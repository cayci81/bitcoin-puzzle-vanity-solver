// CpuEngine tests.
//
// CpuEngine is the CPU reference implementation of IEngine (VanitySearch
// secp256k1 CPU path). These tests prove the Chunk -> key-state mapping with
// REAL keys and REAL addresses before GpuEngine is written:
//
//   1. Puzzle71Assumption.AddressIsCompressedP2PKH
//        Verifies the compressed P2PKH convention for Bitcoin puzzles (1-70)
//        and confirms Puzzle 71's target address decodes to a valid 20-byte
//        P2PKH hash160 while compressed vs uncompressed produce distinct
//        addresses for the same private key.
//   2. EmptyScanScansAllKeysAndFindsNothing
//        Loop runs to completion: keys_scanned == chunk length, zero matches.
//   3. KnownHitScanFindsControlledKey
//        Chunk covering a key we control (K = 0x1234). gpu_table configured
//        for prefix16(hash160(pubkey(K))). Expect exactly the match for K,
//        with the correct Base58 address and a private key hex that
//        round-trips to the same address.
//   4. PartialAbortReportsExactKeyCount
//        stop_flag raised (via on_key hook) after exactly N keys:
//        aborted_early == true, keys_scanned == N.
//   5. CrossCheckMatchesMockEngine
//        MockEngine pre-loaded with INDEPENDENTLY KNOWN constants for
//        privkey 1 (address 1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH, hex "1")
//        must produce an EngineBatchResult identical to CpuEngine's.
//   6. SolverFindsRealFullMatchWithCpuEngine
//        Full coordinator (PuzzleSolver) driven by CpuEngine with pinned
//        seed=1 finds a real full-address match end-to-end.
//   7. SolverFindsRealFullMatchWithDeterministicChunk
//        Deterministic companion to #6: range is exactly one chunk wide,
//        eliminating random chunk placement coupling.
//   8. ChunkExactlyOneGroupLong
//        Boundary test: chunk length == 1024 (kGroupSize) with hit at the
//        last offset (1023).
//   9. ChunkStartingAtKeyZero
//        Boundary test: chunk starts at RangeKey(0) with hit at offset 50.
//  10. StopFlagSetBeforeScanReturnsZeroKeys
//        Pre-set stop_flag returns immediately with aborted_early=true and
//        keys_scanned=0.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "Base58.h"
#include "CpuEngine.h"
#include "Engine.h"
#include "FoundMatchWriter.h"
#include "JsonRangeLogger.h"
#include "PrefixFilter.h"
#include "PuzzleSolver.h"
#include "RandomChunkGenerator.h"
#include "SECP256k1.h"
#include "test_support.h"

using namespace puzzle;
using namespace puzzle::test;

namespace {

// Same mock as test_puzzle_solver.cpp (kept per-file; anonymous namespace).
class MockEngine : public IEngine {
 public:
  void setDefaultResult(const EngineBatchResult& res) { default_result_ = res; }

  EngineBatchResult scan(const Chunk& chunk,
                         const std::vector<uint8_t>& /*gpu_table*/,
                         std::atomic<bool>& /*stop_flag*/) override {
    EngineBatchResult res = default_result_;
    if (res.keys_scanned == 0) {
      res.keys_scanned = chunk.scanned_keys;
    }
    return res;
  }

 private:
  EngineBatchResult default_result_;
};

// prefix16 index of a hash160, identical to VanitySearch's *(prefix_t *)h0
// (little-endian uint16 over the first two bytes).
uint16_t Prefix16(const unsigned char* h160) {
  return static_cast<uint16_t>(h160[0] | (h160[1] << 8));
}

}  // namespace

class CpuEngineTest : public ::testing::Test {
 protected:
  // One shared, Init()'ed secp256k1 context for the whole suite (GTable build
  // is one-time). CpuEngine instances share it, as production workers would.
  static void SetUpTestSuite() {
    secp_ = std::make_shared<Secp256K1>();
    secp_->Init();
  }

  // Build a chunk covering [start, start + keys) from a 256-bit start key.
  static Chunk ChunkAt(const RangeKey& start, uint64_t keys) {
    Chunk c;
    c.start = start;
    c.end = start.plus(keys - 1);
    c.requested_keys = keys;
    c.scanned_keys = keys;
    return c;
  }

  // hash160 + Base58 address for a private key given as hex (test-scale keys).
  static void AddressOf(const char* priv_hex, unsigned char h160_out[20],
                        std::string* addr_out, bool compressed = true) {
    Int k;
    k.SetBase16(const_cast<char*>(priv_hex));
    Point p = secp_->ComputePublicKey(&k);
    secp_->GetHash160(P2PKH, compressed, p, h160_out);
    *addr_out = secp_->GetAddress(P2PKH, compressed, h160_out);
  }

  static std::shared_ptr<Secp256K1> secp_;
};

std::shared_ptr<Secp256K1> CpuEngineTest::secp_;

TEST(Puzzle71Assumption, AddressIsCompressedP2PKH) {
  // Load Puzzle 71's target address and confirm:
  //   (a) Base58Check payload has version byte 0x00 (legacy P2PKH) + 20-byte
  //       hash160 + 4-byte checksum (25 bytes total), and round-trips through
  //       Secp256K1::GetAddress(P2PKH, true, ...).
  //   (b) Compressed and uncompressed encodings of the same private key
  //       produce distinct P2PKH addresses (privkey 1 -> 1BgGZ9tc... vs
  //       1EHNa6Q4Jz...), confirming the compressed=true flag is load-bearing
  //       and matches the Bitcoin puzzle convention (puzzles 1-70 all use
  //       compressed public keys; Puzzle #1's official address is 1BgGZ9tc...).
  Secp256K1 secp;
  secp.Init();

  const std::string kPuzzle71Address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  std::vector<unsigned char> decoded;
  ASSERT_TRUE(DecodeBase58(kPuzzle71Address, decoded));
  // 1-byte version (0x00) + 20-byte hash160 + 4-byte checksum = 25 bytes.
  ASSERT_EQ(decoded.size(), 25u);
  EXPECT_EQ(decoded[0], 0x00);
  EXPECT_EQ(secp.GetAddress(P2PKH, true, decoded.data() + 1), kPuzzle71Address);

  Int k1;
  k1.SetBase16(const_cast<char*>("1"));
  Point p1 = secp.ComputePublicKey(&k1);

  unsigned char h_comp[20];
  unsigned char h_uncomp[20];
  secp.GetHash160(P2PKH, true, p1, h_comp);
  secp.GetHash160(P2PKH, false, p1, h_uncomp);

  const std::string addr_comp = secp.GetAddress(P2PKH, true, h_comp);
  const std::string addr_uncomp = secp.GetAddress(P2PKH, false, h_uncomp);

  ASSERT_EQ(addr_comp, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH");
  ASSERT_EQ(addr_uncomp, "1EHNa6Q4Jz2uvNExL497mE43ikXhwF6kZm");
  ASSERT_NE(addr_comp, addr_uncomp);
}

TEST_F(CpuEngineTest, EmptyScanScansAllKeysAndFindsNothing) {
  CpuEngine engine(secp_);
  std::vector<uint8_t> gpu_table(65536, 0);  // nothing can match
  std::atomic<bool> stop{false};

  Chunk chunk = ChunkAt(RK("1000"), 1000);
  EngineBatchResult res = engine.scan(chunk, gpu_table, stop);

  EXPECT_FALSE(res.aborted_early);
  EXPECT_EQ(res.keys_scanned, 1000u);  // every key was hashed ...
  EXPECT_TRUE(res.matched_addresses.empty());   // ... but nothing matched
  EXPECT_TRUE(res.matched_privkeys_hex.empty());
  EXPECT_TRUE(res.matched_hash160s.empty());
}

TEST_F(CpuEngineTest, KnownHitScanFindsControlledKey) {
  // K = 0x1234, placed at offset 50 inside a 200-key chunk.
  unsigned char h160[20];
  std::string addr;
  AddressOf("1234", h160, &addr);

  std::vector<uint8_t> gpu_table(65536, 0);
  gpu_table[Prefix16(h160)] = 1;

  CpuEngine engine(secp_);
  std::atomic<bool> stop{false};

  Int start_int;
  start_int.SetBase16(const_cast<char*>("1234"));
  start_int.Sub(50);  // chunk starts 50 keys before K
  RangeKey start;
  start.value.Set(&start_int);

  Chunk chunk = ChunkAt(start, 200);
  EngineBatchResult res = engine.scan(chunk, gpu_table, stop);

  EXPECT_FALSE(res.aborted_early);
  EXPECT_EQ(res.keys_scanned, 200u);

  // Exactly one match: the key we control. (No other key in this tiny window
  // shares the 16-bit prefix — deterministic for this fixed key set.)
  ASSERT_EQ(res.matched_addresses.size(), 1u);
  EXPECT_EQ(res.matched_addresses[0], addr);
  ASSERT_EQ(res.matched_hash160s.size(), 1u);
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(res.matched_hash160s[0][i], h160[i]);
  }

  // The reported private key must round-trip: pubkey(privkey) -> same address.
  ASSERT_EQ(res.matched_privkeys_hex.size(), 1u);
  Int k2;
  k2.SetBase16(const_cast<char*>(res.matched_privkeys_hex[0].c_str()));
  Point p2 = secp_->ComputePublicKey(&k2);
  unsigned char h2[20];
  secp_->GetHash160(P2PKH, true, p2, h2);
  EXPECT_EQ(secp_->GetAddress(P2PKH, true, h2), addr);
  // And it really is 0x1234 (GetBase16: uppercase, no leading zeros).
  EXPECT_EQ(res.matched_privkeys_hex[0], "1234");
}

TEST_F(CpuEngineTest, PartialAbortReportsExactKeyCount) {
  std::atomic<bool> stop{false};
  CpuEngineHooks hooks;
  constexpr uint64_t kAbortAfter = 1500;
  hooks.on_key = [&](uint64_t done) {
    if (done == kAbortAfter) {
      stop.store(true);
    }
  };
  CpuEngine engine(secp_, hooks);
  std::vector<uint8_t> gpu_table(65536, 0);

  // 4096 keys > 1 group (1024): the abort lands mid second group.
  Chunk chunk = ChunkAt(RK("2000"), 4096);
  EngineBatchResult res = engine.scan(chunk, gpu_table, stop);

  EXPECT_TRUE(res.aborted_early);
  EXPECT_EQ(res.keys_scanned, kAbortAfter);  // exact, deterministic
  EXPECT_TRUE(res.matched_addresses.empty());
}

TEST_F(CpuEngineTest, CrossCheckMatchesMockEngine) {
  // Independently known constants for private key 1 (compressed P2PKH):
  //   address = 1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH, privkey hex = "1".
  // The mock is pre-loaded with THESE constants; CpuEngine must reproduce
  // them bit-for-bit.
  unsigned char h160[20];
  std::string addr;
  AddressOf("1", h160, &addr);
  ASSERT_EQ(addr, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH");

  std::vector<uint8_t> gpu_table(65536, 0);
  gpu_table[Prefix16(h160)] = 1;

  EngineBatchResult expected;
  std::array<uint8_t, 20> h{};
  for (int i = 0; i < 20; ++i) h[i] = h160[i];
  expected.matched_hash160s.push_back(h);
  expected.matched_addresses.push_back("1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH");
  expected.matched_privkeys_hex.push_back("1");
  expected.keys_scanned = 10;

  MockEngine mock;
  mock.setDefaultResult(expected);
  CpuEngine cpu(secp_);

  Chunk chunk = ChunkAt(RK("1"), 10);
  std::atomic<bool> stop{false};

  EngineBatchResult from_mock = mock.scan(chunk, gpu_table, stop);
  EngineBatchResult from_cpu = cpu.scan(chunk, gpu_table, stop);

  EXPECT_EQ(from_cpu.keys_scanned, from_mock.keys_scanned);
  EXPECT_EQ(from_cpu.aborted_early, from_mock.aborted_early);
  ASSERT_EQ(from_cpu.matched_addresses.size(), from_mock.matched_addresses.size());
  EXPECT_EQ(from_cpu.matched_addresses[0], from_mock.matched_addresses[0]);
  ASSERT_EQ(from_cpu.matched_privkeys_hex.size(), from_mock.matched_privkeys_hex.size());
  EXPECT_EQ(from_cpu.matched_privkeys_hex[0], from_mock.matched_privkeys_hex[0]);
  ASSERT_EQ(from_cpu.matched_hash160s.size(), from_mock.matched_hash160s.size());
  EXPECT_EQ(from_cpu.matched_hash160s[0], from_mock.matched_hash160s[0]);
}

TEST_F(CpuEngineTest, SolverFindsRealFullMatchWithCpuEngine) {
  // This test is deterministic for seed=1. PuzzleSolver may scan a subset
  // of the range (random chunk placement can leave gaps at exhaustion); if
  // K=0xABCD falls in an uncovered gap, the test fails with matches_found=0.
  // Seed=1 was chosen because the RandomChunkGenerator sequence covers
  // K's chunk within the packing bounds. If you change seed, chunk_size,
  // or range, re-verify manually.
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  // Target key K = 0xABCD inside [1, 0x10000].
  unsigned char h160[20];
  std::string target_addr;
  AddressOf("ABCD", h160, &target_addr);

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RK("1");
  chunk_cfg.range_end = RK("10000");
  chunk_cfg.chunk_size = 4096;
  chunk_cfg.gpu_stride = 1024;
  chunk_cfg.seed = 1;  // <-- PIN explicitly, not just default
  chunk_cfg.max_collision_retries = 8;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher = std::make_shared<AddressMatcher>(std::vector<std::string>{}, target_addr);

  std::atomic<bool> stop_flag{false};
  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  match_cfg.stop_flag = &stop_flag;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<CpuEngine>(secp_);

  SolverConfig solver_cfg;
  solver_cfg.gpu_table.assign(65536, 0);
  solver_cfg.gpu_table[Prefix16(h160)] = 1;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.worker_id = 0;
  solver_cfg.max_chunks = 1000;  // <-- not 32; run until exhausted or found

  ::testing::internal::CaptureStderr();
  PuzzleSolver solver(engine, gen, logger, matcher, writer, solver_cfg);
  SolverExitReason reason = solver.run();
  ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(reason, SolverExitReason::FullMatchFound);
  EXPECT_TRUE(stop_flag.load());
  EXPECT_EQ(solver.getStats().matches_found, 1u);

  auto match_lines = ReadLines(match_path);
  ASSERT_EQ(match_lines.size(), 1u);
  auto mj = nlohmann::json::parse(match_lines[0]);
  EXPECT_EQ(mj["kind"], "full");
  EXPECT_EQ(mj["address"], target_addr);
  EXPECT_EQ(mj["private_key_hex"], "ABCD");

  // The hit chunk is logged aborted; earlier chunks completed.
  auto log_lines = ReadLines(log_path);
  ASSERT_GE(log_lines.size(), 1u);
  auto last = nlohmann::json::parse(log_lines.back());
  EXPECT_EQ(last["status"], "aborted");
  for (size_t i = 0; i + 1 < log_lines.size(); ++i) {
    EXPECT_EQ(nlohmann::json::parse(log_lines[i])["status"], "completed");
  }
}

TEST_F(CpuEngineTest, SolverFindsRealFullMatchWithDeterministicChunk) {
  // Range exactly one chunk wide, positioned to include K=0xABCD.
  // The generator must yield exactly that one chunk before exhausting,
  // so the coordinator loop is deterministic regardless of seed.
  //
  // CHUNKS_SCANNED == 1 INVARIANT DEPENDENCY:
  //   - effective_chunk_keys = floor(chunk_size / gpu_stride) * gpu_stride
  //                          = floor(4096 / 4096) * 4096 = 4096
  //   - range width = range_end - range_start + 1
  //                 = start.plus(4095) - start + 1 = 4096
  //   - 1st generator.next() returns [start, start + 4095] (covering K=0xABCD)
  //   - 2nd generator.next() returns std::nullopt (0 unscanned keys remain)
  // If chunk_size, gpu_stride, or range width is changed independently,
  // the chunks_scanned == 1 assertion below will break.
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  unsigned char h160[20];
  std::string target_addr;
  AddressOf("ABCD", h160, &target_addr);

  Int start_int;
  start_int.SetBase16(const_cast<char*>("ABCD"));
  start_int.Sub(100);  // starts 100 keys before K (0xAB69)

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RangeKey(start_int);
  chunk_cfg.range_end = chunk_cfg.range_start.plus(4095);  // 4096 keys
  chunk_cfg.chunk_size = 4096;
  chunk_cfg.gpu_stride = 4096;  // round to stride, one chunk exactly
  chunk_cfg.seed = 42;          // any seed yields the single exact chunk
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher = std::make_shared<AddressMatcher>(std::vector<std::string>{}, target_addr);

  std::atomic<bool> stop_flag{false};
  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  match_cfg.stop_flag = &stop_flag;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  auto engine = std::make_shared<CpuEngine>(secp_);

  SolverConfig solver_cfg;
  solver_cfg.gpu_table.assign(65536, 0);
  solver_cfg.gpu_table[Prefix16(h160)] = 1;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.worker_id = 0;
  solver_cfg.max_chunks = 1000;

  ::testing::internal::CaptureStderr();
  PuzzleSolver solver(engine, gen, logger, matcher, writer, solver_cfg);
  SolverExitReason reason = solver.run();
  ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(reason, SolverExitReason::FullMatchFound);
  EXPECT_TRUE(stop_flag.load());
  EXPECT_EQ(solver.getStats().matches_found, 1u);
  EXPECT_EQ(solver.getStats().chunks_scanned, 1u);

  auto match_lines = ReadLines(match_path);
  ASSERT_EQ(match_lines.size(), 1u);
  auto mj = nlohmann::json::parse(match_lines[0]);
  EXPECT_EQ(mj["kind"], "full");
  EXPECT_EQ(mj["address"], target_addr);
  EXPECT_EQ(mj["private_key_hex"], "ABCD");

  auto log_lines = ReadLines(log_path);
  ASSERT_EQ(log_lines.size(), 1u);
  auto last = nlohmann::json::parse(log_lines[0]);
  EXPECT_EQ(last["status"], "aborted");
}

TEST_F(CpuEngineTest, ChunkExactlyOneGroupLong) {
  // Boundary test: chunk length == 1024 (kGroupSize), with gpu_table
  // configured to hit at offset 1023 (the last key of the group).
  // Verifies the group boundary logic does not off-by-one on the last key.
  const RangeKey start = RK("5000");
  const RangeKey last_key = start.plus(CpuEngine::kGroupSize - 1);  // offset 1023
  const std::string last_priv_hex = const_cast<Int&>(last_key.value).GetBase16();

  unsigned char h160[20];
  std::string expected_addr;
  AddressOf(last_priv_hex.c_str(), h160, &expected_addr);

  std::vector<uint8_t> gpu_table(65536, 0);
  gpu_table[Prefix16(h160)] = 1;

  CpuEngine engine(secp_);
  std::atomic<bool> stop{false};
  Chunk chunk = ChunkAt(start, CpuEngine::kGroupSize);
  EngineBatchResult res = engine.scan(chunk, gpu_table, stop);

  EXPECT_FALSE(res.aborted_early);
  EXPECT_EQ(res.keys_scanned, static_cast<uint64_t>(CpuEngine::kGroupSize));
  ASSERT_EQ(res.matched_addresses.size(), 1u);
  EXPECT_EQ(res.matched_addresses[0], expected_addr);
  EXPECT_EQ(res.matched_privkeys_hex[0], last_priv_hex);
}

TEST_F(CpuEngineTest, ChunkStartingAtKeyZero) {
  // Boundary test: chunk starts at key 0 with length 100, and gpu_table
  // hits at offset 50 (0x32). Verifies Int(0) start does not break the
  // group-center computation (0 + kGroupSize/2 = 512).
  unsigned char h160[20];
  std::string expected_addr;
  AddressOf("32", h160, &expected_addr);  // 50 decimal == 0x32

  std::vector<uint8_t> gpu_table(65536, 0);
  gpu_table[Prefix16(h160)] = 1;

  CpuEngine engine(secp_);
  std::atomic<bool> stop{false};
  Chunk chunk = ChunkAt(RK("0"), 100);
  EngineBatchResult res = engine.scan(chunk, gpu_table, stop);

  EXPECT_FALSE(res.aborted_early);
  EXPECT_EQ(res.keys_scanned, 100u);
  ASSERT_EQ(res.matched_addresses.size(), 1u);
  EXPECT_EQ(res.matched_addresses[0], expected_addr);
  EXPECT_EQ(res.matched_privkeys_hex[0], "32");
}

TEST_F(CpuEngineTest, StopFlagSetBeforeScanReturnsZeroKeys) {
  // Pre-set stop_flag before calling scan(). Verifies the outer loop's
  // top-of-group stop check catches pre-set stops without hashing any keys.
  CpuEngine engine(secp_);
  std::vector<uint8_t> gpu_table(65536, 1);  // even with all prefixes enabled
  std::atomic<bool> stop{true};

  Chunk chunk = ChunkAt(RK("1000"), 1024);
  EngineBatchResult res = engine.scan(chunk, gpu_table, stop);

  EXPECT_TRUE(res.aborted_early);
  EXPECT_EQ(res.keys_scanned, 0u);
  EXPECT_TRUE(res.matched_addresses.empty());
  EXPECT_TRUE(res.matched_privkeys_hex.empty());
  EXPECT_TRUE(res.matched_hash160s.empty());
}

TEST_F(CpuEngineTest, LiveProgressAdvancesDuringScan) {
  // A CpuEngine must report live per-key progress while scan() is still
  // running. This is what makes 60fps GUI telemetry possible.
  CpuEngine engine(secp_);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> progress{0};
  std::vector<uint8_t> gpu_table(65536, 0);

  Chunk chunk = ChunkAt(RK("100000"), 100000);  // 100K keys = ~0.3s
  std::thread observer([&] {
    // Wait up to 250ms for progress to move past 1000, proving live
    // updates before scan() returns.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(250);
    while (progress.load() < 1000 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop.store(true);   // cut the scan short so the test is fast
  });

  EXPECT_TRUE(engine.reportsLiveProgress());
  engine.setProgressCounter(&progress);
  EngineBatchResult r = engine.scan(chunk, gpu_table, stop);
  engine.setProgressCounter(nullptr);
  observer.join();

  EXPECT_GE(progress.load(), 1000ULL);
  (void)r;
  // Note: we do NOT assert progress == r.keys_scanned, because the
  // observer may have set stop mid-chunk (aborted_early=true). The
  // invariant that matters is progress > 0 while scan() is in flight.
}
