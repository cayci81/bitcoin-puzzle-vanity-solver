// FoundMatchWriter tests.
//
// Requirements:
//   R1  Output file: found_matches.jsonl, append-only, one JSON object per line.
//   R2  Prefix match (hex or Base58): line has address, kind="prefix", matched_prefix,
//       timestamp, gpu_id. It MUST NOT contain a private key field.
//   R3  Full-address match: line additionally contains private_key_hex (kind="full").
//   R4  On a full match the private key is written to BOTH found_matches.jsonl AND stderr,
//       and fsync()/FlushFileBuffers() has returned BEFORE the writer returns control.
//   R5  After a full match the stop flag is raised and all GPU worker threads halt cleanly
//       (no further chunk is dequeued, no further scanned_ranges record is marked
//       "completed" for in-flight chunks - they are logged as "aborted").
//   R6  Concurrent matches from several GPU threads never interleave within a line.
//   R7  A failure to write/fsync the full-match record must not be swallowed: the process
//       falls back to stderr and exits non-zero.
//   R8  Unwritable output path -> constructor throws (fail fast, before any scanning).

#include <gtest/gtest.h>

#include <atomic>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Engine.h"
#include "FoundMatchWriter.h"
#include "JsonRangeLogger.h"
#include "PuzzleSolver.h"
#include "RandomChunkGenerator.h"
#include "test_support.h"

#define TDD_SKIP(reason) GTEST_SKIP() << "TDD_SKIP: " << (reason)

using puzzle::FoundMatchRecord;
using puzzle::FoundMatchWriter;
using puzzle::MatchKind;
using puzzle::MatchWriterConfig;

TEST(FoundMatchWriter, UnwritablePathThrowsInConstructor) {
  // R8: Directory path cannot be opened for append -> throws std::runtime_error
  puzzle::test::TempDir temp_dir;
  MatchWriterConfig cfg;
  cfg.output_path = temp_dir.path();
  EXPECT_THROW(FoundMatchWriter writer(cfg), std::runtime_error);
}

TEST(FoundMatchWriter, PrefixMatchRecordHasNoPrivateKey) {
  // R2: Prefix match record must omit private_key_hex completely (key absent, not "")
  puzzle::test::TempDir temp_dir;
  MatchWriterConfig cfg;
  cfg.output_path = temp_dir.path() / "found_matches.jsonl";
  FoundMatchWriter writer(cfg);

  FoundMatchRecord rec;
  rec.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  rec.kind = MatchKind::Prefix;
  rec.matched_prefix = "1PWo";
  rec.private_key_hex = "SHOULD_NOT_APPEAR";
  rec.gpu_id = 0;

  writer.recordMatch(rec);

  auto lines = puzzle::test::ReadLines(cfg.output_path);
  ASSERT_EQ(lines.size(), 1u);

  nlohmann::json j = nlohmann::json::parse(lines[0]);
  EXPECT_FALSE(j.contains("private_key_hex"));
  EXPECT_EQ(j["kind"], "prefix");
  EXPECT_EQ(j["address"], rec.address);
  EXPECT_EQ(j["matched_prefix"], "1PWo");
  EXPECT_EQ(j["gpu_id"], 0);
  EXPECT_FALSE(j["timestamp"].get<std::string>().empty());
}

TEST(FoundMatchWriter, FullMatchRecordContainsPrivateKey) {
  // R3: Full match record must include private_key_hex and kind="full"
  puzzle::test::TempDir temp_dir;
  std::atomic<bool> stop_flag{false};
  MatchWriterConfig cfg;
  cfg.output_path = temp_dir.path() / "found_matches.jsonl";
  cfg.stop_flag = &stop_flag;
  FoundMatchWriter writer(cfg);

  FoundMatchRecord rec;
  rec.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  rec.kind = MatchKind::Full;
  rec.matched_prefix = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  rec.private_key_hex = "400000000000000000123456789abcdef";
  rec.gpu_id = 1;

  ::testing::internal::CaptureStderr();
  writer.recordMatch(rec);
  std::string captured = ::testing::internal::GetCapturedStderr();
  EXPECT_NE(captured.find("private_key_hex"), std::string::npos);

  EXPECT_TRUE(stop_flag.load());

  auto lines = puzzle::test::ReadLines(cfg.output_path);
  ASSERT_EQ(lines.size(), 1u);

  nlohmann::json j = nlohmann::json::parse(lines[0]);
  EXPECT_TRUE(j.contains("private_key_hex"));
  EXPECT_EQ(j["private_key_hex"], "400000000000000000123456789abcdef");
  EXPECT_EQ(j["kind"], "full");
  EXPECT_EQ(j["address"], rec.address);
  EXPECT_EQ(j["gpu_id"], 1);
  EXPECT_FALSE(j["timestamp"].get<std::string>().empty());
}

TEST(FoundMatchWriter, FullMatchIsWrittenToFileAndStderrAndSyncedBeforeReturn) {
  // R4: sync_hook is invoked after data is written & flushed to OS buffer
  puzzle::test::TempDir temp_dir;
  std::atomic<bool> sync_called{false};
  std::atomic<bool> file_had_content_in_hook{false};
  auto out_path = temp_dir.path() / "found_matches.jsonl";

  MatchWriterConfig cfg;
  cfg.output_path = out_path;
  cfg.sync_hook = [&](int fd) {
    (void)fd;
    sync_called.store(true);
    auto lines = puzzle::test::ReadLines(out_path);
    if (!lines.empty() && lines[0].find("private_key_hex") != std::string::npos) {
      file_had_content_in_hook.store(true);
    }
  };

  FoundMatchWriter writer(cfg);

  FoundMatchRecord rec;
  rec.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  rec.kind = MatchKind::Full;
  rec.matched_prefix = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  rec.private_key_hex = "deadbeefcafebabe";
  rec.gpu_id = 0;

  ::testing::internal::CaptureStderr();
  writer.recordMatch(rec);
  std::string captured = ::testing::internal::GetCapturedStderr();
  (void)captured;

  EXPECT_TRUE(sync_called.load());
  EXPECT_TRUE(file_had_content_in_hook.load());

  auto final_lines = puzzle::test::ReadLines(out_path);
  ASSERT_EQ(final_lines.size(), 1u);
  nlohmann::json j = nlohmann::json::parse(final_lines[0]);
  EXPECT_EQ(j["private_key_hex"], "deadbeefcafebabe");
}

TEST(FoundMatchWriter, FullMatchRaisesStopFlagAndHaltsWorkersCleanly) {
  // R5: After a full match the stop flag is raised and workers halt cleanly.
  puzzle::test::TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  puzzle::GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = puzzle::test::RK("400000000000000000");
  chunk_cfg.range_end = puzzle::test::RK("400000001000000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<puzzle::RandomChunkGenerator>(chunk_cfg);

  puzzle::LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<puzzle::JsonRangeLogger>(log_cfg);

  std::string target_addr = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  auto matcher_ptr = std::make_shared<puzzle::AddressMatcher>(std::vector<std::string>{}, target_addr);

  std::atomic<bool> stop_flag{false};
  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  match_cfg.stop_flag = &stop_flag;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  class MockHaltingEngine : public puzzle::IEngine {
   public:
    puzzle::EngineBatchResult scan(const puzzle::Chunk& chunk,
                                   const std::vector<uint8_t>&,
                                   std::atomic<bool>&) override {
      calls++;
      puzzle::EngineBatchResult res;
      res.keys_scanned = chunk.scanned_keys;
      if (calls == 1) {
        res.matched_addresses.push_back("1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU");
        res.matched_privkeys_hex.push_back("400000000000000000abcdef123456");
      }
      return res;
    }
    int calls = 0;
  };

  auto engine = std::make_shared<MockHaltingEngine>();

  puzzle::SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.max_chunks = 10;

  ::testing::internal::CaptureStderr();
  puzzle::PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  puzzle::SolverExitReason reason = solver.run();
  ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(reason, puzzle::SolverExitReason::FullMatchFound);
  EXPECT_TRUE(stop_flag.load());
  EXPECT_EQ(engine->calls, 1);
}

TEST(FoundMatchWriter, ConcurrentWritesDoNotInterleave) {
  // R6: 8 threads x 100 writes must all produce valid, non-interleaved JSON lines
  puzzle::test::TempDir temp_dir;
  auto out_path = temp_dir.path() / "concurrent_matches.jsonl";

  MatchWriterConfig cfg;
  cfg.output_path = out_path;
  FoundMatchWriter writer(cfg);

  constexpr int kThreads = 8;
  constexpr int kWritesPerThread = 100;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&writer, t, kWritesPerThread]() {
      for (int i = 0; i < kWritesPerThread; ++i) {
        FoundMatchRecord rec;
        rec.address = "addr_t" + std::to_string(t) + "_i" + std::to_string(i);
        rec.kind = MatchKind::Prefix;
        rec.matched_prefix = "1P";
        rec.gpu_id = t;
        writer.recordMatch(rec);
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  auto lines = puzzle::test::ReadLines(out_path);
  ASSERT_EQ(lines.size(), static_cast<size_t>(kThreads * kWritesPerThread));

  for (size_t idx = 0; idx < lines.size(); ++idx) {
    ASSERT_NO_THROW({
      nlohmann::json j = nlohmann::json::parse(lines[idx]);
      EXPECT_EQ(j["kind"], "prefix");
      EXPECT_FALSE(j["address"].get<std::string>().empty());
    }) << "Line " << idx << " failed to parse as valid JSON: " << lines[idx];
  }
}

TEST(FoundMatchWriter, WriteFailureOnFullMatchFallsBackToStderrAndFailsLoudly) {
  // R7: When write fails, stderr already received key, stop_flag is raised,
  // and std::runtime_error is thrown with address only (no key).
  puzzle::test::TempDir temp_dir;
  std::atomic<bool> stop_flag{false};

  MatchWriterConfig cfg;
  cfg.output_path = temp_dir.path() / "matches.jsonl";
  cfg.stop_flag = &stop_flag;
  cfg.write_hook = [](const std::string&) {
    throw std::runtime_error("simulated disk error");
  };

  FoundMatchWriter writer(cfg);

  FoundMatchRecord full_match;
  full_match.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  full_match.kind = MatchKind::Full;
  full_match.matched_prefix = full_match.address;
  full_match.private_key_hex = "secret_key_1234567890abcdef";
  full_match.gpu_id = 2;

  std::stringstream captured_stderr;
  auto* old_buf = std::cerr.rdbuf(captured_stderr.rdbuf());

  try {
    writer.recordMatch(full_match);
    std::cerr.rdbuf(old_buf);
    FAIL() << "recordMatch should have thrown on write failure";
  } catch (const std::runtime_error& ex) {
    std::cerr.rdbuf(old_buf);  // restore cerr
    EXPECT_TRUE(stop_flag.load());
    std::string err_str = captured_stderr.str();
    EXPECT_NE(err_str.find("secret_key_1234567890abcdef"), std::string::npos);
    // Ensure exception message contains address but NOT the private key
    std::string msg = ex.what();
    EXPECT_NE(msg.find(full_match.address), std::string::npos);
    EXPECT_EQ(msg.find("secret_key_1234567890abcdef"), std::string::npos);
  } catch (...) {
    std::cerr.rdbuf(old_buf);
    FAIL() << "Expected std::runtime_error";
  }
}
