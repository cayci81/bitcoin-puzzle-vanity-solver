#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "CpuEngine.h"
#include "Engine.h"
#include "FoundMatchWriter.h"
#include "JsonRangeLogger.h"
#include "PrefixFilter.h"
#include "PuzzleSolver.h"
#include "RandomChunkGenerator.h"
#include "SECP256K1.h"
#include "test_support.h"

using namespace puzzle;
using namespace puzzle::test;

namespace {

class MockEngine : public IEngine {
 public:
  void setChunkResult(size_t chunk_idx, const EngineBatchResult& res) {
    behaviors_[chunk_idx] = res;
  }

  void setDefaultResult(const EngineBatchResult& res) {
    default_result_ = res;
  }

  EngineBatchResult scan(const Chunk& chunk,
                         const std::vector<uint8_t>& /*gpu_table*/,
                         std::atomic<bool>& /*stop_flag*/) override {
    size_t idx = call_count_++;
    auto it = behaviors_.find(idx);
    EngineBatchResult res = (it != behaviors_.end()) ? it->second : default_result_;
    if (res.keys_scanned == 0) {
      res.keys_scanned = chunk.scanned_keys;
    }
    return res;
  }

  size_t callCount() const { return call_count_.load(); }

 private:
  std::atomic<size_t> call_count_{0};
  std::map<size_t, EngineBatchResult> behaviors_;
  EngineBatchResult default_result_;
};

}  // namespace

TEST(PuzzleSolverTest, EmptyResultScansNChunksAndLogsAllCompleted) {
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000000500000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{}, std::nullopt);

  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();

  std::atomic<bool> stop_flag{false};
  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.worker_id = 3;
  solver_cfg.max_chunks = 5;

  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  SolverExitReason reason = solver.run();

  EXPECT_EQ(reason, SolverExitReason::Exhausted);
  auto stats = solver.getStats();
  EXPECT_EQ(stats.chunks_scanned, 5u);
  EXPECT_EQ(stats.matches_found, 0u);

  auto lines = ReadLines(log_path);
  ASSERT_EQ(lines.size(), 5u);
  for (const auto& line : lines) {
    auto j = nlohmann::json::parse(line);
    EXPECT_EQ(j["status"], "completed");
    EXPECT_EQ(j["gpu_id"], 3);
  }
}

TEST(PuzzleSolverTest, PrefixHitWritesPrefixMatchAndContinues) {
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000001000000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{"1PWo"}, std::nullopt);

  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();
  // Chunk index 2 (3rd chunk) yields a prefix hit
  EngineBatchResult prefix_hit;
  prefix_hit.matched_addresses.push_back("1PWo999999999999999999999999999999");
  prefix_hit.matched_privkeys_hex.push_back("dummy_privkey_prefix");
  engine->setChunkResult(2, prefix_hit);

  std::atomic<bool> stop_flag{false};
  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.max_chunks = 4;

  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  SolverExitReason reason = solver.run();

  EXPECT_EQ(reason, SolverExitReason::Exhausted);
  EXPECT_FALSE(stop_flag.load());
  EXPECT_EQ(solver.getStats().matches_found, 1u);

  auto match_lines = ReadLines(match_path);
  ASSERT_EQ(match_lines.size(), 1u);
  auto mj = nlohmann::json::parse(match_lines[0]);
  EXPECT_EQ(mj["kind"], "prefix");
  EXPECT_EQ(mj["matched_prefix"], "1PWo");
  EXPECT_FALSE(mj.contains("private_key_hex"));

  auto log_lines = ReadLines(log_path);
  ASSERT_EQ(log_lines.size(), 4u);
  for (const auto& l : log_lines) {
    auto j = nlohmann::json::parse(l);
    EXPECT_EQ(j["status"], "completed");
  }
}

TEST(PuzzleSolverTest, FullHitOnChunkTwoLogsAbortedAndHaltsWorkers) {
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000001000000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  std::string target_addr = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{}, target_addr);

  std::atomic<bool> stop_flag{false};
  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  match_cfg.stop_flag = &stop_flag;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();
  // Chunk index 1 (2nd chunk) yields the target address
  EngineBatchResult full_hit;
  full_hit.matched_addresses.push_back(target_addr);
  full_hit.matched_privkeys_hex.push_back("400000000000000000deadbeef123456");
  engine->setChunkResult(1, full_hit);

  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.max_chunks = 5;

  ::testing::internal::CaptureStderr();
  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  SolverExitReason reason = solver.run();
  ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(reason, SolverExitReason::FullMatchFound);
  EXPECT_TRUE(stop_flag.load());
  EXPECT_EQ(engine->callCount(), 2u);  // Chunks 3..5 never executed
  EXPECT_EQ(solver.getStats().matches_found, 1u);

  auto match_lines = ReadLines(match_path);
  ASSERT_EQ(match_lines.size(), 1u);
  auto mj = nlohmann::json::parse(match_lines[0]);
  EXPECT_EQ(mj["kind"], "full");
  EXPECT_EQ(mj["address"], target_addr);
  EXPECT_EQ(mj["private_key_hex"], "400000000000000000deadbeef123456");

  auto log_lines = ReadLines(log_path);
  ASSERT_EQ(log_lines.size(), 2u);
  auto j0 = nlohmann::json::parse(log_lines[0]);
  EXPECT_EQ(j0["status"], "completed");
  auto j1 = nlohmann::json::parse(log_lines[1]);
  EXPECT_EQ(j1["status"], "aborted");
}

TEST(PuzzleSolverTest, FullHitOnOnlyChunkLogsAborted) {
  // Hit chunk itself is interrupted and logged as "aborted"
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000000100000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  std::string target_addr = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{}, target_addr);

  std::atomic<bool> stop_flag{false};
  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  match_cfg.stop_flag = &stop_flag;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();
  EngineBatchResult full_hit;
  full_hit.matched_addresses.push_back(target_addr);
  full_hit.matched_privkeys_hex.push_back("40000000000000000011112222333344");
  engine->setChunkResult(0, full_hit);

  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.max_chunks = 1;

  ::testing::internal::CaptureStderr();
  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  SolverExitReason reason = solver.run();
  ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(reason, SolverExitReason::FullMatchFound);
  EXPECT_TRUE(stop_flag.load());

  auto log_lines = ReadLines(log_path);
  ASSERT_EQ(log_lines.size(), 1u);
  auto j0 = nlohmann::json::parse(log_lines[0]);
  EXPECT_EQ(j0["status"], "aborted");
}

TEST(PuzzleSolverTest, FullHitOnActualLastChunkNoExtraAbortedRecords) {
  // max_chunks = 3; chunks 0 and 1 are empty (completed); chunk 2 hits Full (aborted).
  // Total logged = 3 records (2 completed, 1 aborted), exactly 0 extra aborted records.
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000000500000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  std::string target_addr = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{}, target_addr);

  std::atomic<bool> stop_flag{false};
  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  match_cfg.stop_flag = &stop_flag;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();
  EngineBatchResult full_hit;
  full_hit.matched_addresses.push_back(target_addr);
  full_hit.matched_privkeys_hex.push_back("40000000000000000033333333333333");
  engine->setChunkResult(2, full_hit);

  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.max_chunks = 3;

  ::testing::internal::CaptureStderr();
  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  SolverExitReason reason = solver.run();
  ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(reason, SolverExitReason::FullMatchFound);
  EXPECT_TRUE(stop_flag.load());

  auto log_lines = ReadLines(log_path);
  ASSERT_EQ(log_lines.size(), 3u);
  EXPECT_EQ(nlohmann::json::parse(log_lines[0])["status"], "completed");
  EXPECT_EQ(nlohmann::json::parse(log_lines[1])["status"], "completed");
  EXPECT_EQ(nlohmann::json::parse(log_lines[2])["status"], "aborted");
}

TEST(PuzzleSolverTest, GeneratorExhaustionExitsCleanly) {
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  // Exactly 2 chunks in range
  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000000200000000");
  chunk_cfg.chunk_size = 0x100000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{}, std::nullopt);

  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();

  std::atomic<bool> stop_flag{false};
  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.max_chunks = 100;

  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  SolverExitReason reason = solver.run();

  EXPECT_EQ(reason, SolverExitReason::Exhausted);
  auto log_lines = ReadLines(log_path);
  EXPECT_GT(log_lines.size(), 0u);
  for (const auto& l : log_lines) {
    auto j = nlohmann::json::parse(l);
    EXPECT_EQ(j["status"], "completed");
  }
}

TEST(PuzzleSolverTest, StoppedExternalBeforeRunExitsImmediately) {
  // stop_flag set to true BEFORE run() -> zero chunks scanned, zero engine calls
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000000500000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{}, std::nullopt);

  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();

  std::atomic<bool> stop_flag{true};  // Already stopped before run
  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.max_chunks = 5;

  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  SolverExitReason reason = solver.run();

  EXPECT_EQ(reason, SolverExitReason::StoppedExternal);
  EXPECT_EQ(engine->callCount(), 0u);
  EXPECT_EQ(ReadLines(log_path).size(), 0u);
  EXPECT_EQ(solver.getStats().chunks_scanned, 0u);
}

TEST(PuzzleSolverTest, PrefixMatchDoesNotLeakPrivateKey) {
  // Engine populates matched_privkeys_hex for all matches.
  // Solver MUST strip it when classification is Prefix.
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000000200000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{"1PWo"}, std::nullopt);

  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();
  EngineBatchResult res;
  res.matched_addresses.push_back("1PWoPrefixOnlyMatch1234567890123");
  // Engine returns private key, but match is only Prefix
  res.matched_privkeys_hex.push_back("SHOULD_NEVER_BE_IN_JSON_FILE");
  engine->setChunkResult(0, res);

  std::atomic<bool> stop_flag{false};
  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.max_chunks = 1;

  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  solver.run();

  auto match_lines = ReadLines(match_path);
  ASSERT_EQ(match_lines.size(), 1u);
  auto mj = nlohmann::json::parse(match_lines[0]);
  EXPECT_EQ(mj["kind"], "prefix");
  EXPECT_FALSE(mj.contains("private_key_hex"));
}

TEST(PuzzleSolverTest, ThreadSafetyTwoSolversShareWriter) {
  TempDir temp_dir;
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  auto shared_writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{"1P"}, std::nullopt);

  auto run_worker = [&](int id) {
    auto log_path = temp_dir.path() / ("scanned_ranges_w" + std::to_string(id) + ".jsonl");
    GeneratorConfig chunk_cfg;
    chunk_cfg.range_start = RK("400000000000000000").plus(static_cast<uint64_t>(id) * 0x100000000ULL);
    chunk_cfg.range_end = chunk_cfg.range_start.plus(0x100000000ULL);
    chunk_cfg.chunk_size = 0x10000000ULL;
    chunk_cfg.gpu_stride = 262144;
    auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

    LoggerConfig log_cfg;
    log_cfg.directory = temp_dir.path();
    log_cfg.base_name = "scanned_ranges_w" + std::to_string(id);
    log_cfg.flush_every_n = 1;
    auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

    auto engine = std::make_shared<MockEngine>();
    EngineBatchResult hit;
    hit.matched_addresses.push_back("1Pworker" + std::to_string(id));
    engine->setDefaultResult(hit);

    std::atomic<bool> stop_flag{false};
    SolverConfig solver_cfg;
    solver_cfg.stop_flag = &stop_flag;
    solver_cfg.worker_id = id;
    solver_cfg.max_chunks = 10;

    PuzzleSolver solver(engine, gen, logger, matcher_ptr, shared_writer, solver_cfg);
    solver.run();
  };

  std::thread t1([&]() { run_worker(1); });
  std::thread t2([&]() { run_worker(2); });
  t1.join();
  t2.join();

  auto match_lines = ReadLines(match_path);
  EXPECT_EQ(match_lines.size(), 20u);
  for (const auto& l : match_lines) {
    ASSERT_NO_THROW({
      auto j = nlohmann::json::parse(l);
      EXPECT_EQ(j["kind"], "prefix");
    });
  }
}

TEST(PuzzleSolverTest, AbortedEarlyChunkLogsAbortedAndReturnsStoppedExternal) {
  // Engine reports aborted_early=true (stop_flag raised externally mid-chunk,
  // no full match). The interrupted chunk must be logged status="aborted" and
  // run() must return StoppedExternal — NOT FullMatchFound.
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000000500000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{}, std::nullopt);

  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();
  EngineBatchResult aborted;
  aborted.aborted_early = true;               // engine stopped before completing the chunk
  aborted.keys_scanned = 0x8000000ULL;        // partial progress still counts
  engine->setChunkResult(1, aborted);         // chunk 0 completes, chunk 1 aborts

  std::atomic<bool> stop_flag{false};
  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.worker_id = 2;
  solver_cfg.max_chunks = 5;

  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  SolverExitReason reason = solver.run();

  EXPECT_EQ(reason, SolverExitReason::StoppedExternal);
  EXPECT_EQ(engine->callCount(), 2u);  // chunks 2..4 never executed
  EXPECT_EQ(solver.getStats().keys_scanned,
            0x10000000ULL + 0x8000000ULL);   // completed chunk + partial chunk

  auto log_lines = ReadLines(log_path);
  ASSERT_EQ(log_lines.size(), 2u);
  auto j0 = nlohmann::json::parse(log_lines[0]);
  EXPECT_EQ(j0["status"], "completed");
  EXPECT_EQ(j0["gpu_id"], 2);
  EXPECT_EQ(j0["requested_keys"], 0x10000000ULL);
  EXPECT_EQ(j0["scanned_keys"], 0x10000000ULL);   // completed: full chunk scanned
  auto j1 = nlohmann::json::parse(log_lines[1]);
  EXPECT_EQ(j1["status"], "aborted");
  EXPECT_EQ(j1["gpu_id"], 2);
  // Self-describing abort record: requested = full chunk, scanned = actual work.
  // Resume logic can continue at start + 0x8000000.
  EXPECT_EQ(j1["requested_keys"], 0x10000000ULL);
  EXPECT_EQ(j1["scanned_keys"], 0x8000000ULL);
}

TEST(PuzzleSolverTest, CompletedChunkReportsScannedKeysEqualToChunkSize) {
  // Explicit invariant in the other direction: for status=completed,
  // scanned_keys MUST equal the chunk's effective length (and requested_keys
  // the configured chunk_size). Resume logic relies on this equality.
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000000500000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher_ptr = std::make_shared<AddressMatcher>(std::vector<std::string>{}, std::nullopt);

  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();  // honest engine: full chunk per scan

  std::atomic<bool> stop_flag{false};
  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.max_chunks = 3;

  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  SolverExitReason reason = solver.run();

  EXPECT_EQ(reason, SolverExitReason::Exhausted);
  EXPECT_EQ(solver.getStats().keys_scanned, 3u * 0x10000000ULL);

  auto log_lines = ReadLines(log_path);
  ASSERT_EQ(log_lines.size(), 3u);
  for (const auto& l : log_lines) {
    auto j = nlohmann::json::parse(l);
    EXPECT_EQ(j["status"], "completed");
    EXPECT_EQ(j["requested_keys"], 0x10000000ULL);
    EXPECT_EQ(j["scanned_keys"], 0x10000000ULL);
  }
}

TEST(PuzzleSolverTest, LiveKeysScannedAdvancesWithinAChunk) {
  // With CpuEngine + a mid-sized chunk, getStats().keys_scanned must
  // move past zero WHILE the first chunk is still scanning, not only
  // at chunk boundaries.
  TempDir temp_dir;
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  auto secp = std::make_shared<Secp256K1>();
  secp->Init();

  // 262,144-key chunk takes ~750ms at ~350K/s in Debug mode.
  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("100000");
  chunk_cfg.range_end = RK("8FFFFF");
  chunk_cfg.chunk_size = 262144;
  chunk_cfg.gpu_stride = 262144;
  chunk_cfg.seed = 1;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher = std::make_shared<AddressMatcher>(
      std::vector<std::string>{}, "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU");

  std::atomic<bool> stop_flag{false};
  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  match_cfg.stop_flag = &stop_flag;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<CpuEngine>(secp);

  SolverConfig solver_cfg;
  solver_cfg.gpu_table.assign(65536, 0);
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.worker_id = 0;
  solver_cfg.max_chunks = 10;

  PuzzleSolver solver(engine, gen, logger, matcher, writer, solver_cfg);

  SolverExitReason exit_reason = SolverExitReason::Exhausted;
  std::thread worker([&]() {
    exit_reason = solver.run();
  });

  // Poll getStats() every 20ms up to 200ms (with a fallback up to 2s on
  // heavily loaded CI) and verify keys_scanned > 0 while chunks_scanned == 0.
  const auto t0 = std::chrono::steady_clock::now();
  const auto deadline = t0 + std::chrono::milliseconds(2000);
  SolverStats live_stats{};
  while (std::chrono::steady_clock::now() < deadline) {
    live_stats = solver.getStats();
    if (live_stats.keys_scanned > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const auto elapsed_to_first_progress_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();

  EXPECT_GT(live_stats.keys_scanned, 0ULL);
  EXPECT_EQ(live_stats.chunks_scanned, 0ULL);
  EXPECT_LE(elapsed_to_first_progress_ms, 200);

  stop_flag.store(true, std::memory_order_release);
  worker.join();

  EXPECT_EQ(exit_reason, SolverExitReason::StoppedExternal);
}

TEST(PuzzleSolverTest, TelemetryFreezesAfterRun) {
  TempDir temp_dir;
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("400000000000000000");
  chunk_cfg.range_end = RK("400000000200000000");
  chunk_cfg.chunk_size = 0x10000000ULL;
  chunk_cfg.gpu_stride = 262144;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher_ptr =
      std::make_shared<AddressMatcher>(std::vector<std::string>{}, std::nullopt);

  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<MockEngine>();

  std::atomic<bool> stop_flag{false};
  SolverConfig solver_cfg;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.max_chunks = 2;

  PuzzleSolver solver(engine, gen, logger, matcher_ptr, writer, solver_cfg);
  EXPECT_EQ(solver.run(), SolverExitReason::Exhausted);

  SolverStats stats1 = solver.getStats();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  SolverStats stats2 = solver.getStats();

  EXPECT_DOUBLE_EQ(stats1.elapsed_seconds, stats2.elapsed_seconds);
  EXPECT_DOUBLE_EQ(stats1.rate_keys_per_sec, stats2.rate_keys_per_sec);
}
