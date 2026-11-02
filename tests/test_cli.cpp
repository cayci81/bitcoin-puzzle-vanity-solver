#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../cli.h"
#include "../JsonRangeLogger.h"
#include "../PrefixFilter.h"
#include "../SECP256K1.h"

namespace fs = std::filesystem;
using namespace puzzle;
using namespace puzzle::cli;

namespace {

class TempDir {
 public:
  TempDir() {
    auto base = fs::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto candidate = base / ("puzzle71_cli_test_" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
          std::to_string(i));
      std::error_code ec;
      if (fs::create_directory(candidate, ec)) {
        path_ = candidate;
        return;
      }
    }
    throw std::runtime_error("Failed to create temporary directory");
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

ParsedArgs CallParseArgs(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  std::string prog = "puzzle71_solver";
  argv.push_back(prog.data());
  std::vector<std::string> mutable_args = args;
  for (auto& s : mutable_args) {
    argv.push_back(s.data());
  }
  return ParseArgs(static_cast<int>(argv.size()), argv.data());
}

std::string CompressedAddressForHexKey(const char* hex_privkey) {
  Secp256K1 secp;
  secp.Init();
  Int k;
  k.SetBase16(const_cast<char*>(hex_privkey));
  Point pub = secp.ComputePublicKey(&k);
  return secp.GetAddress(P2PKH, true, pub);
}

std::string Hash160Prefix4HexForHexKey(const char* hex_privkey) {
  Secp256K1 secp;
  secp.Init();
  Int k;
  k.SetBase16(const_cast<char*>(hex_privkey));
  Point pub = secp.ComputePublicKey(&k);
  unsigned char h[20] = {0};
  secp.GetHash160(P2PKH, true, pub, h);
  char buf[5];
  std::snprintf(buf, sizeof(buf), "%02x%02x", h[0], h[1]);
  return std::string(buf);
}

std::vector<nlohmann::json> ReadJsonLines(const fs::path& file_path) {
  std::vector<nlohmann::json> out;
  std::ifstream in(file_path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      out.push_back(nlohmann::json::parse(line));
    }
  }
  return out;
}

}  // namespace

// ============================================================================
// ParseArgs Unit Tests
// ============================================================================

TEST(CliParseArgsTest, MinimalInvocationSetsFieldsAndDefaults) {
  auto parsed = CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "400000000000000000:7fffffffffffffffff"
  });

  EXPECT_FALSE(parsed.show_help);
  EXPECT_FALSE(parsed.show_version);
  const auto& opts = parsed.options;
  EXPECT_EQ(opts.address, "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU");
  EXPECT_EQ(opts.range_start.ToHex(), "400000000000000000");
  EXPECT_EQ(opts.range_end.ToHex(), "7fffffffffffffffff");
  EXPECT_EQ(opts.chunk_size, 100'000'000ULL);
  EXPECT_EQ(opts.gpu_stride, 262144ULL);
  EXPECT_EQ(opts.max_collision_retries, 8U);
  EXPECT_EQ(opts.seed, 0ULL);
  EXPECT_EQ(opts.output_dir, fs::path("."));
  EXPECT_EQ(opts.engine, "cpu");
  EXPECT_EQ(opts.gpu_id, 0);
  EXPECT_EQ(opts.max_chunks, 0ULL);
  EXPECT_FALSE(opts.use_endomorphism);
}

TEST(CliParseArgsTest, ChunkSizeOverridesDefault) {
  auto parsed = CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--chunk-size", "50000000"
  });
  EXPECT_EQ(parsed.options.chunk_size, 50'000'000ULL);
}

TEST(CliParseArgsTest, SeedParsedAsUint64) {
  auto parsed = CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--seed", "42"
  });
  EXPECT_EQ(parsed.options.seed, 42ULL);
}

TEST(CliParseArgsTest, PrefixHexValidatedByHexPrefixFilter) {
  auto parsed = CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--prefix-hex", "5abc,6123"
  });
  EXPECT_EQ(parsed.options.prefix_hex_csv, "5abc,6123");

  HexPrefixFilter filter = HexPrefixFilter::parse(parsed.options.prefix_hex_csv);
  ASSERT_EQ(filter.prefixes().size(), 2U);
  EXPECT_EQ(filter.prefixes()[0], "5abc");
  EXPECT_EQ(filter.prefixes()[1], "6123");

  EXPECT_THROW(CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--prefix-hex", "0x5abc"
  }), std::invalid_argument);
}

TEST(CliParseArgsTest, PrefixB58RepeatableAndValidatedByAddressMatcher) {
  auto parsed = CallParseArgs({
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--prefix-b58", "1PWo3",
      "--prefix-b58", "1BgGZ"
  });
  ASSERT_EQ(parsed.options.prefix_b58.size(), 2U);
  EXPECT_EQ(parsed.options.prefix_b58[0], "1PWo3");
  EXPECT_EQ(parsed.options.prefix_b58[1], "1BgGZ");

  EXPECT_THROW(CallParseArgs({
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--prefix-b58", "1PWo0"
  }), std::invalid_argument);
}

TEST(CliParseArgsTest, OutputDirSetCorrectly) {
  auto parsed = CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--output-dir", "/tmp/foo"
  });
  EXPECT_EQ(parsed.options.output_dir, fs::path("/tmp/foo"));
}

TEST(CliParseArgsTest, EngineCpuAndInvalidEngine) {
  auto parsed_cpu = CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--engine", "cpu"
  });
  EXPECT_EQ(parsed_cpu.options.engine, "cpu");

  EXPECT_THROW(CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--engine", "tpu"
  }), std::invalid_argument);
}

TEST(CliParseArgsTest, ParsesEngineGpuAccepted) {
  // ParseArgs must accept "--engine gpu" without throwing even on a non-CUDA
  // build; CUDA availability is checked in RunSolver, not ParseArgs.
  auto parsed_gpu = CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--engine", "gpu"
  });
  EXPECT_EQ(parsed_gpu.options.engine, "gpu");
}

TEST(CliParseArgsTest, MaxChunksParsedCorrectly) {
  auto parsed = CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "400000000000000000:7fffffffffffffffff",
      "--max-chunks", "16"
  });
  EXPECT_EQ(parsed.options.max_chunks, 16ULL);
}

TEST(CliParseArgsTest, HelpFlagShortCircuitsImmediately) {
  auto help_parsed = CallParseArgs({"--help", "--unknown-flag"});
  EXPECT_TRUE(help_parsed.show_help);
  EXPECT_FALSE(help_parsed.show_version);
}

TEST(CliParseArgsTest, VersionFlagShortCircuitsImmediately) {
  auto ver_parsed = CallParseArgs({"--version", "--unknown-flag"});
  EXPECT_TRUE(ver_parsed.show_version);
  EXPECT_FALSE(ver_parsed.show_help);
}

TEST(CliParseArgsTest, MissingRangeHexThrows) {
  EXPECT_THROW(CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU"
  }), std::invalid_argument);
}

TEST(CliParseArgsTest, InvalidRangeHexMissingColonThrows) {
  EXPECT_THROW(CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "4000000000000000007fffffffffffffffff"
  }), std::invalid_argument);
}

TEST(CliParseArgsTest, InvalidRangeHexBadHexThrows) {
  EXPECT_THROW(CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "ZZZZ:FFFF"
  }), std::invalid_argument);
}

TEST(CliParseArgsTest, InvalidRangeHexStartGreaterThanEndThrows) {
  EXPECT_THROW(CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "ABFF:AB00"
  }), std::invalid_argument);
}

TEST(CliParseArgsTest, InvalidChunkSizeZeroThrows) {
  EXPECT_THROW(CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "AB00:ABFF",
      "--chunk-size", "0"
  }), std::invalid_argument);
}

TEST(CliParseArgsTest, InvalidChunkSizeNonNumericThrows) {
  EXPECT_THROW(CallParseArgs({
      "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
      "--range-hex", "AB00:ABFF",
      "--chunk-size", "not_a_number"
  }), std::invalid_argument);
}

TEST(CliParseArgsTest, UnknownFlagThrowsWithFlagName) {
  try {
    CallParseArgs({
        "--address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
        "--range-hex", "AB00:ABFF",
        "--bogus-flag"
    });
    FAIL() << "Expected std::invalid_argument on unknown flag";
  } catch (const std::invalid_argument& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("--bogus-flag"), std::string::npos);
  }
}

// ============================================================================
// RunSolver & End-to-End Integration Tests
// ============================================================================

TEST(CliIntegrationTest, RunSolverFindsKnownKey) {
  TempDir temp_dir;
  const std::string target_addr = CompressedAddressForHexKey("ABCD");

  // Multi-chunk range [0x1, 0x10000] with seed = 1, chunk_size = 4096,
  // gpu_stride = 1024 so that >= 1 chunks complete ("completed") before the
  // chunk containing 0xABCD is found and logged as "aborted".
  CliOptions options;
  options.range_start = RangeKey::FromHex("1");
  options.range_end = RangeKey::FromHex("10000");
  options.chunk_size = 4096;
  options.gpu_stride = 1024;
  options.seed = 1;
  options.address = target_addr;
  options.engine = "cpu";
  options.output_dir = temp_dir.path();

  ::testing::internal::CaptureStderr();
  RunResult r = RunSolver(options);
  ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.reason, SolverExitReason::FullMatchFound);
  EXPECT_EQ(r.stats.matches_found, 1ULL);

  auto match_lines = ReadJsonLines(options.output_dir / "found_matches.jsonl");
  ASSERT_EQ(match_lines.size(), 1U);
  EXPECT_EQ(match_lines[0].at("kind").get<std::string>(), "full");
  EXPECT_EQ(match_lines[0].at("address").get<std::string>(), target_addr);
  EXPECT_EQ(match_lines[0].at("private_key_hex").get<std::string>(), "ABCD");

  auto range_lines = ReadJsonLines(options.output_dir / "scanned_ranges.jsonl");
  ASSERT_GE(range_lines.size(), 2U);
  size_t completed_count = 0;
  size_t aborted_count = 0;
  for (const auto& j : range_lines) {
    std::string status = j.at("status").get<std::string>();
    if (status == "completed") ++completed_count;
    if (status == "aborted") ++aborted_count;
  }
  EXPECT_GE(completed_count, 1U);
  EXPECT_EQ(aborted_count, 1U);
}

TEST(CliIntegrationTest, RunSolverEmptyGpuTableWithFullAddressIsFiltered) {
  TempDir temp_dir;
  const std::string target_addr = CompressedAddressForHexKey("ABCD");

  CliOptions options;
  options.range_start = RangeKey::FromHex("AB00");
  options.range_end = RangeKey::FromHex("ABFF");
  options.chunk_size = 256;
  options.gpu_stride = 256;
  options.seed = 1;
  options.address = target_addr;
  options.engine = "cpu";
  options.output_dir = temp_dir.path();

  ::testing::internal::CaptureStderr();
  RunResult r = RunSolver(options);
  std::string captured = ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.reason, SolverExitReason::FullMatchFound);
  EXPECT_NE(captured.find("gpu_table  : 1 active"), std::string::npos)
      << "Regression: empty prefix_hex_csv with a full --address "
         "must narrow gpu_table to exactly the target hash160's "
         "prefix16, NOT leave it as all-ones (65536). An all-ones "
         "table makes CpuEngine Base58-encode every key — for "
         "Puzzle #71 this is 5 keys/s and the range never finishes.\n"
         "Actual stderr:\n"
      << captured;
}

TEST(CliIntegrationTest, RunSolverChunkSmallerThanStrideThrows) {
  TempDir temp_dir;

  CliOptions options;
  options.range_start = RangeKey::FromHex("AB00");
  options.range_end = RangeKey::FromHex("AEFF");
  options.chunk_size = 256;
  options.gpu_stride = 512;
  options.seed = 1;
  options.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  options.engine = "cpu";
  options.output_dir = temp_dir.path();

  EXPECT_THROW(RunSolver(options), std::invalid_argument);
}

#ifndef PUZZLE71_BUILD_CUDA
TEST(CliIntegrationTest, RunSolverInvalidEngineOnNonCudaBuild) {
  TempDir temp_dir;

  CliOptions options;
  options.range_start = RangeKey::FromHex("AB00");
  options.range_end = RangeKey::FromHex("ABFF");
  options.chunk_size = 256;
  options.gpu_stride = 256;
  options.seed = 1;
  options.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  options.engine = "gpu";
  options.output_dir = temp_dir.path();

  try {
    RunSolver(options);
    FAIL() << "Expected std::invalid_argument when running engine='gpu' on non-CUDA build";
  } catch (const std::invalid_argument& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("GPU engine not compiled in"), std::string::npos)
        << "Actual message: " << msg;
  }
}
#endif

TEST(CliIntegrationTest, RunSolverResumeSkipsScannedRanges) {
  TempDir temp_dir;
  const std::string target_addr = CompressedAddressForHexKey("ABCD");

  // Range [0xAB00, 0xABFF] (256 keys) partitioned into four 64-key chunks:
  //   chunk 0: [0xAB00, 0xAB3F]
  //   chunk 1: [0xAB40, 0xAB7F]
  //   chunk 2: [0xAB80, 0xABBF]
  //   chunk 3: [0xABC0, 0xABFF]  <-- contains 0xABCD
  {
    LoggerConfig log_cfg;
    log_cfg.directory = temp_dir.path();
    log_cfg.flush_every_n = 1;
    JsonRangeLogger pre_logger(log_cfg);

    const char* starts[3] = {"AB00", "AB40", "AB80"};
    const char* ends[3] = {"AB3F", "AB7F", "ABBF"};
    for (int i = 0; i < 3; ++i) {
      RangeRecord rec;
      rec.start = RangeKey::FromHex(starts[i]);
      rec.end = RangeKey::FromHex(ends[i]);
      rec.requested_keys = 64;
      rec.scanned_keys = 64;
      rec.status = RangeStatus::Completed;
      ASSERT_TRUE(pre_logger.append(rec));
    }
    pre_logger.flush();
  }

  CliOptions options;
  options.range_start = RangeKey::FromHex("AB00");
  options.range_end = RangeKey::FromHex("ABFF");
  options.chunk_size = 64;
  options.gpu_stride = 64;
  options.seed = 42;
  options.address = target_addr;
  options.engine = "cpu";
  options.output_dir = temp_dir.path();

  ::testing::internal::CaptureStderr();
  RunResult res = RunSolver(options);
  ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(res.exit_code, 0);
  EXPECT_EQ(res.reason, SolverExitReason::FullMatchFound);
  EXPECT_EQ(res.stats.chunks_scanned, 1ULL);
  EXPECT_EQ(res.stats.matches_found, 1ULL);

  auto after_lines = ReadJsonLines(options.output_dir / "scanned_ranges.jsonl");
  ASSERT_EQ(after_lines.size(), 4U);
  EXPECT_EQ(after_lines[0].at("start_hex").get<std::string>(), "ab00");
  EXPECT_EQ(after_lines[1].at("start_hex").get<std::string>(), "ab40");
  EXPECT_EQ(after_lines[2].at("start_hex").get<std::string>(), "ab80");
  EXPECT_EQ(after_lines[3].at("start_hex").get<std::string>(), "abc0");
  EXPECT_EQ(after_lines[3].at("end_hex").get<std::string>(), "abff");
}

TEST(CliIntegrationTest, RunSolverHexPrefixAloneStillWorks) {
  TempDir temp_dir;
  const std::string target_addr = CompressedAddressForHexKey("ABCD");
  const std::string hex_pfx4 = Hash160Prefix4HexForHexKey("ABCD");
  const std::string b58_pfx = target_addr.substr(0, 5);

  CliOptions options;
  options.range_start = RangeKey::FromHex("AB00");
  options.range_end = RangeKey::FromHex("ABFF");
  options.chunk_size = 256;
  options.gpu_stride = 256;
  options.seed = 1;
  options.prefix_hex_csv = hex_pfx4;
  options.prefix_b58 = {b58_pfx};
  options.engine = "cpu";
  options.output_dir = temp_dir.path();

  ::testing::internal::CaptureStderr();
  RunResult res = RunSolver(options);
  std::string captured = ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(res.exit_code, 0);
  EXPECT_EQ(res.reason, SolverExitReason::Exhausted);
  EXPECT_GE(res.stats.matches_found, 1ULL);
  EXPECT_NE(captured.find("gpu_table  : 1 active"), std::string::npos)
      << "Expected 1 active prefix in gpu_table for 4-nibble --prefix-hex";

  auto match_lines = ReadJsonLines(options.output_dir / "found_matches.jsonl");
  ASSERT_GE(match_lines.size(), 1U);
  bool found_abcd_prefix = false;
  for (const auto& j : match_lines) {
    EXPECT_EQ(j.at("kind").get<std::string>(), "prefix");
    EXPECT_EQ(j.at("matched_prefix").get<std::string>(), b58_pfx);
    EXPECT_FALSE(j.contains("private_key_hex") &&
                 !j.at("private_key_hex").get<std::string>().empty());
    if (j.at("address").get<std::string>() == target_addr) {
      found_abcd_prefix = true;
    }
  }
  EXPECT_TRUE(found_abcd_prefix);
}

TEST(CliIntegrationTest, SignalHandlingStopFlagExits130AndLogsAbortedRecord) {
  TempDir temp_dir;

  CliOptions options;
  options.range_start = RangeKey::FromHex("100000");
  options.range_end = RangeKey::FromHex("8FFFFF");
  options.chunk_size = 524288;
  options.gpu_stride = 524288;
  options.seed = 1;
  options.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  options.engine = "cpu";
  options.output_dir = temp_dir.path();

  std::atomic<bool> external_stop{false};
  std::thread stopper([&external_stop]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    external_stop.store(true, std::memory_order_release);
  });

  ::testing::internal::CaptureStderr();
  RunResult res = RunSolver(options, &external_stop);
  ::testing::internal::GetCapturedStderr();
  stopper.join();

  EXPECT_EQ(res.exit_code, 130);
  EXPECT_EQ(res.reason, SolverExitReason::StoppedExternal);

  auto range_lines = ReadJsonLines(options.output_dir / "scanned_ranges.jsonl");
  ASSERT_GE(range_lines.size(), 1U);
  const auto& last_rec = range_lines.back();
  EXPECT_EQ(last_rec.at("status").get<std::string>(), "aborted");
  EXPECT_GT(last_rec.at("scanned_keys").get<uint64_t>(), 0ULL);
  EXPECT_LT(last_rec.at("scanned_keys").get<uint64_t>(), options.chunk_size);
}

TEST(CliIntegrationTest, RunSolverInvalidAddressChecksumThrows) {
  TempDir temp_dir;

  CliOptions options;
  options.range_start = RangeKey::FromHex("AB00");
  options.range_end = RangeKey::FromHex("ABFF");
  options.chunk_size = 256;
  options.gpu_stride = 256;
  // Valid length (25 decoded bytes) and Base58 alphabet, but last character
  // modified ('U' -> 'V') so double-SHA256 Base58Check checksum fails.
  options.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXV";
  options.engine = "cpu";
  options.output_dir = temp_dir.path();

  EXPECT_THROW(RunSolver(options), std::invalid_argument);
}

namespace {
std::vector<std::string> ReadTextLines(const fs::path& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) out.push_back(line);
  }
  return out;
}
}  // namespace

TEST(CliIntegrationTest, AddressFileWithMultipleTargetsFindsKnownKey) {
  TempDir temp_dir;
  const std::string target_addr = CompressedAddressForHexKey("ABCD");
  const std::string other_addr = CompressedAddressForHexKey("1234");
  const auto list = temp_dir.path() / "addrs.txt";
  {
    std::ofstream out(list);
    out << "# targets\n" << other_addr << "\n\n" << target_addr << "\n";
  }

  CliOptions options;
  options.range_start = RangeKey::FromHex("AB00");
  options.range_end = RangeKey::FromHex("ABFF");
  options.chunk_size = 256;
  options.gpu_stride = 256;
  options.seed = 1;
  options.address_file = list;
  options.engine = "cpu";
  options.output_dir = temp_dir.path() / "out";

  ::testing::internal::CaptureStderr();
  RunResult res = RunSolver(options);
  std::string captured = ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(res.reason, SolverExitReason::FullMatchFound);
  EXPECT_NE(captured.find("gpu_table  : 2 active"), std::string::npos) << captured;
  auto lines = ReadJsonLines(options.output_dir / "found_matches.jsonl");
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines[0].at("address").get<std::string>(), target_addr);
  EXPECT_EQ(lines[0].at("private_key_hex").get<std::string>(), "ABCD");
}

TEST(CliIntegrationTest, PrefixFileProducesPrefixFoundTxt) {
  TempDir temp_dir;
  const std::string target_addr = CompressedAddressForHexKey("ABCD");
  const std::string hex_pfx4 = Hash160Prefix4HexForHexKey("ABCD");
  const std::string b58_pfx = target_addr.substr(0, 5);
  const auto list = temp_dir.path() / "prefixes.txt";
  {
    std::ofstream out(list);
    out << "# vanity\n" << hex_pfx4 << "\n" << b58_pfx << "\n";
  }

  CliOptions options;
  options.range_start = RangeKey::FromHex("AB00");
  options.range_end = RangeKey::FromHex("ABFF");
  options.chunk_size = 256;
  options.gpu_stride = 256;
  options.seed = 1;
  options.prefix_file = list;
  options.engine = "cpu";
  options.output_dir = temp_dir.path() / "out";

  ::testing::internal::CaptureStderr();
  RunResult res = RunSolver(options);
  ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(res.reason, SolverExitReason::Exhausted);
  auto txt = ReadTextLines(options.output_dir / "prefix_found.txt");
  ASSERT_GE(txt.size(), 1u);
  bool saw_abcd = false;
  for (const auto& l : txt) {
    if (l.find("Address: " + target_addr) != std::string::npos &&
        l.find("Matched Prefix: " + b58_pfx) != std::string::npos &&
        l.find("Private Key: ABCD") != std::string::npos) {
      saw_abcd = true;
    }
  }
  EXPECT_TRUE(saw_abcd);
}

TEST(CliIntegrationTest, BadAddressFileLineThrowsRuntimeErrorWithLineNumber) {
  TempDir temp_dir;
  const auto list = temp_dir.path() / "bad.txt";
  {
    std::ofstream out(list);
    out << "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU\nnot-an-address\n";
  }
  CliOptions options;
  options.range_start = RangeKey::FromHex("AB00");
  options.range_end = RangeKey::FromHex("ABFF");
  options.chunk_size = 256;
  options.gpu_stride = 256;
  options.address_file = list;
  options.output_dir = temp_dir.path() / "out";
  try {
    RunSolver(options);
    FAIL() << "expected std::runtime_error";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("line 2"), std::string::npos);
  }
}

TEST(CliParseArgsTest, AddressFileAndPrefixFileFlagsAreParsed) {
  TempDir temp_dir;
  const auto a = temp_dir.path() / "a.txt";
  const auto p = temp_dir.path() / "p.txt";
  {
    std::ofstream(a) << "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU\n";
    std::ofstream(p) << "5abc\n1PWo\n";
  }
  std::string as = a.string(), ps = p.string();
  std::vector<std::string> args = {"prog", "--range-hex", "AB00:ABFF",
                                   "--address-file", as, "--prefix-file", ps};
  std::vector<char*> argv;
  for (auto& s : args) argv.push_back(s.data());
  ParsedArgs parsed = ParseArgs(static_cast<int>(argv.size()), argv.data());
  EXPECT_EQ(parsed.options.address_file, a);
  EXPECT_EQ(parsed.options.prefix_file, p);
}
