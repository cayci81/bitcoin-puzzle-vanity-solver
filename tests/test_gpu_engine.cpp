// GpuEngine tests (cross-checked against CpuEngine as correctness oracle).
//
// Contract coverage:
//   1. DenseGpuTableRejectedAtConstruction
//        gpu_table with popcount > GpuEngine::kMaxGpuTablePopcount (64)
//        throws std::runtime_error at construction time (Option 3).
//   2. GpuMatchesCpuOnEmptyChunk
//        All-zero gpu_table scans full chunk and reports zero matches.
//   3. GpuMatchesCpuOnSingleHit
//        Chunk with exactly 1 matching key: element-by-element equality
//        against CpuEngine is valid and asserted directly.
//   4. GpuMatchesCpuOnMultiHit
//        Chunk with 3+ known matches (mix of even-y and odd-y pubkeys):
//        GPU threads run in parallel and report hits in warp-completion
//        order rather than key-index order. Asserts SET-EQUALITY by sorting
//        both matched vectors before comparing.
//   5. GpuMatchesCpuOnDensePrefixTable
//        Multi-prefix gpu_table (popcount <= kMaxGpuTablePopcount): asserts
//        set-equality against CpuEngine and verifies that no endo != 0 or
//        -k mod n symmetric false positives leak into EngineBatchResult.
//   6. GpuMatchesCpuOnTinyChunk
//        Chunk length N = 100 (<< nbThread * kStepSize): verifies Option C
//        small-chunk partitioning (min(nbThread, ceil(N / STEP_SIZE)) active
//        threads, idle threads masked, out-of-range offsets filtered).
//   7. GpuAbortBetweenSteps
//        Verifies the ABORT GRANULARITY CONTRACT: GpuEngine halts at step
//        boundaries, so keys_scanned satisfies:
//          last_completed_step * step_keys <= keys_scanned
//            < last_completed_step * step_keys + step_keys
//        and aborted_early == true.
//   8. StopFlagSetBeforeScanReturnsZeroKeys
//        Pre-set stop_flag returns immediately with aborted_early == true
//        and keys_scanned == 0.
//   9. SolverFindsRealFullMatchWithGpuEngine
//        End-to-end PuzzleSolver coordinator with GpuEngine on a
//        deterministic chunk containing K = 0xABCD.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "CpuEngine.h"
#include "Engine.h"
#include "FoundMatchWriter.h"
#include "GpuEngine.h"
#include "JsonRangeLogger.h"
#include "PrefixFilter.h"
#include "PuzzleSolver.h"
#include "RandomChunkGenerator.h"
#include "SECP256k1.h"
#include "test_support.h"

using namespace puzzle;
using namespace puzzle::test;

namespace {

uint16_t Prefix16(const unsigned char* h160) {
  return static_cast<uint16_t>(h160[0] | (h160[1] << 8));
}

// Canonical sort helper for set-equality comparisons on multi-hit chunks.
// GPU threads emit matches in warp-completion order rather than key-index
// order, so multi-hit comparisons sort (address, privkey_hex, hash160)
// tuples before asserting equality with CpuEngine.
struct MatchTuple {
  std::string address;
  std::string privkey_hex;
  std::array<uint8_t, 20> hash160;

  bool operator<(const MatchTuple& o) const {
    if (address != o.address) return address < o.address;
    if (privkey_hex != o.privkey_hex) return privkey_hex < o.privkey_hex;
    return hash160 < o.hash160;
  }

  bool operator==(const MatchTuple& o) const {
    return address == o.address && privkey_hex == o.privkey_hex &&
           hash160 == o.hash160;
  }
};

std::vector<MatchTuple> SortedMatches(const EngineBatchResult& res) {
  EXPECT_EQ(res.matched_addresses.size(), res.matched_privkeys_hex.size());
  EXPECT_EQ(res.matched_addresses.size(), res.matched_hash160s.size());
  std::vector<MatchTuple> out;
  out.reserve(res.matched_addresses.size());
  for (size_t i = 0; i < res.matched_addresses.size(); ++i) {
    out.push_back(MatchTuple{res.matched_addresses[i],
                             res.matched_privkeys_hex[i],
                             res.matched_hash160s[i]});
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

class GpuEngineTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    secp_ = std::make_shared<Secp256K1>();
    secp_->Init();
  }

  static Chunk ChunkAt(const RangeKey& start, uint64_t keys) {
    Chunk c;
    c.start = start;
    c.end = start.plus(keys - 1);
    c.requested_keys = keys;
    c.scanned_keys = keys;
    return c;
  }

  static void AddressOf(const char* priv_hex, unsigned char h160_out[20],
                        std::string* addr_out, bool* is_odd_out = nullptr) {
    Int k;
    k.SetBase16(const_cast<char*>(priv_hex));
    Point p = secp_->ComputePublicKey(&k);
    if (is_odd_out != nullptr) {
      *is_odd_out = p.y.IsOdd();
    }
    secp_->GetHash160(P2PKH, true, p, h160_out);
    *addr_out = secp_->GetAddress(P2PKH, true, h160_out);
  }

  static std::shared_ptr<Secp256K1> secp_;
};

std::shared_ptr<Secp256K1> GpuEngineTest::secp_;

TEST_F(GpuEngineTest, DenseGpuTableRejectedAtConstruction) {
  std::vector<uint8_t> dense_table(65536, 0);
  for (size_t i = 0; i <= GpuEngine::kMaxGpuTablePopcount; ++i) {
    dense_table[i] = 1;  // 65 active prefixes > kMaxGpuTablePopcount (64)
  }
  EXPECT_THROW(GpuEngine(secp_, dense_table), std::runtime_error);

  // Boundary: exactly kMaxGpuTablePopcount (64) is accepted.
  dense_table[GpuEngine::kMaxGpuTablePopcount] = 0;
  EXPECT_NO_THROW(GpuEngine(secp_, dense_table));
}

#ifdef PUZZLE71_ENABLE_CUDA

TEST_F(GpuEngineTest, GpuMatchesCpuOnEmptyChunk) {
  std::vector<uint8_t> gpu_table(65536, 0);
  CpuEngine cpu(secp_);
  GpuEngine gpu(secp_, gpu_table);
  std::atomic<bool> stop{false};

  Chunk chunk = ChunkAt(RK("1000"), 4096);
  EngineBatchResult from_cpu = cpu.scan(chunk, gpu_table, stop);
  EngineBatchResult from_gpu = gpu.scan(chunk, gpu_table, stop);

  EXPECT_FALSE(from_gpu.aborted_early);
  EXPECT_EQ(from_gpu.keys_scanned, from_cpu.keys_scanned);
  EXPECT_TRUE(from_gpu.matched_addresses.empty());
  EXPECT_TRUE(from_gpu.matched_privkeys_hex.empty());
  EXPECT_TRUE(from_gpu.matched_hash160s.empty());
}

TEST_F(GpuEngineTest, GpuMatchesCpuOnSingleHit) {
  // Single controlled hit at K = 0x1234 inside a 2048-key chunk.
  // Because there is only 1 hit, element-by-element equality is valid.
  unsigned char h160[20];
  std::string addr;
  AddressOf("1234", h160, &addr);

  std::vector<uint8_t> gpu_table(65536, 0);
  gpu_table[Prefix16(h160)] = 1;

  CpuEngine cpu(secp_);
  GpuEngine gpu(secp_, gpu_table);
  std::atomic<bool> stop{false};

  Chunk chunk = ChunkAt(RK("1000"), 2048);
  EngineBatchResult from_cpu = cpu.scan(chunk, gpu_table, stop);
  EngineBatchResult from_gpu = gpu.scan(chunk, gpu_table, stop);

  EXPECT_FALSE(from_gpu.aborted_early);
  EXPECT_EQ(from_gpu.keys_scanned, from_cpu.keys_scanned);
  ASSERT_EQ(from_gpu.matched_addresses.size(), 1u);
  ASSERT_EQ(from_cpu.matched_addresses.size(), 1u);
  EXPECT_EQ(from_gpu.matched_addresses[0], from_cpu.matched_addresses[0]);
  EXPECT_EQ(from_gpu.matched_privkeys_hex[0], from_cpu.matched_privkeys_hex[0]);
  EXPECT_EQ(from_gpu.matched_hash160s[0], from_cpu.matched_hash160s[0]);
}

TEST_F(GpuEngineTest, GpuMatchesCpuOnMultiHit) {
  // 4 known keys across multiple 1024-key groups, intentionally mixing
  // even-y (privkey 1 -> even y, privkey 2 -> even y) and odd-y pubkeys
  // (privkey 3 -> odd y, privkey 6 -> odd y).
  // GPU threads run in parallel and emit hits in warp-completion order,
  // so we compare sorted match sets (set-equal, NOT index-order identical).
  const char* kKeys[] = {"1", "6", "405", "810"};
  std::vector<uint8_t> gpu_table(65536, 0);
  bool saw_even = false;
  bool saw_odd = false;
  for (const char* kh : kKeys) {
    unsigned char h[20];
    std::string a;
    bool is_odd = false;
    AddressOf(kh, h, &a, &is_odd);
    saw_odd |= is_odd;
    saw_even |= !is_odd;
    gpu_table[Prefix16(h)] = 1;
  }
  ASSERT_TRUE(saw_even && saw_odd);

  CpuEngine cpu(secp_);
  GpuEngine gpu(secp_, gpu_table);
  std::atomic<bool> stop{false};

  Chunk chunk = ChunkAt(RK("1"), 4096);
  EngineBatchResult from_cpu = cpu.scan(chunk, gpu_table, stop);
  EngineBatchResult from_gpu = gpu.scan(chunk, gpu_table, stop);

  EXPECT_FALSE(from_gpu.aborted_early);
  EXPECT_EQ(from_gpu.keys_scanned, from_cpu.keys_scanned);
  ASSERT_GE(from_cpu.matched_addresses.size(), 4u);

  auto cpu_sorted = SortedMatches(from_cpu);
  auto gpu_sorted = SortedMatches(from_gpu);
  ASSERT_EQ(gpu_sorted.size(), cpu_sorted.size());
  EXPECT_EQ(gpu_sorted, cpu_sorted);
}

TEST_F(GpuEngineTest, GpuMatchesCpuOnDensePrefixTable) {
  // Enable kMaxGpuTablePopcount (64) distinct 16-bit prefixes and compare
  // GpuEngine vs CpuEngine via set-equality. Also verifies that no
  // endomorphism (endo != 0) or symmetric (-k mod n) false positives leak.
  std::vector<uint8_t> gpu_table(65536, 0);
  for (size_t i = 0; i < GpuEngine::kMaxGpuTablePopcount; ++i) {
    gpu_table[i * 1021 % 65536] = 1;
  }

  CpuEngine cpu(secp_);
  GpuEngine gpu(secp_, gpu_table);
  std::atomic<bool> stop{false};

  Chunk chunk = ChunkAt(RK("10000"), 8192);
  EngineBatchResult from_cpu = cpu.scan(chunk, gpu_table, stop);
  EngineBatchResult from_gpu = gpu.scan(chunk, gpu_table, stop);

  EXPECT_FALSE(from_gpu.aborted_early);
  EXPECT_EQ(from_gpu.keys_scanned, from_cpu.keys_scanned);
  EXPECT_EQ(SortedMatches(from_gpu), SortedMatches(from_cpu));
}

TEST_F(GpuEngineTest, GpuMatchesCpuOnTinyChunk) {
  // N = 100 keys (<< nbThread * STEP_SIZE): verifies Option C small-chunk
  // handling (min(nbThread, ceil(N / STEP_SIZE)) active threads, idle
  // threads ignored, offsets >= 100 filtered out by [chunk.start, chunk.end]).
  unsigned char h160[20];
  std::string addr;
  AddressOf("1032", h160, &addr);  // offset 0x32 (50) from 0x1000

  std::vector<uint8_t> gpu_table(65536, 0);
  gpu_table[Prefix16(h160)] = 1;

  CpuEngine cpu(secp_);
  GpuEngine gpu(secp_, gpu_table);
  std::atomic<bool> stop{false};

  Chunk chunk = ChunkAt(RK("1000"), 100);
  EngineBatchResult from_cpu = cpu.scan(chunk, gpu_table, stop);
  EngineBatchResult from_gpu = gpu.scan(chunk, gpu_table, stop);

  EXPECT_FALSE(from_gpu.aborted_early);
  EXPECT_EQ(from_gpu.keys_scanned, 100u);
  EXPECT_EQ(from_gpu.keys_scanned, from_cpu.keys_scanned);
  EXPECT_EQ(SortedMatches(from_gpu), SortedMatches(from_cpu));
}

TEST_F(GpuEngineTest, GpuAbortBetweenSteps) {
  // ABORT GRANULARITY & CONTIGUOUS STEP PROGRESS CONTRACT:
  // GpuEngine aborts at kernel step boundaries, and step 0 covers the
  // contiguous range [chunk.start, chunk.start + step_keys).
  // Use 4 threads (step_keys = 4 * 1024 = 4096) across 4 steps (16384 keys):
  //   - Key at offset 3572 (0xDF4, in thread 3 of step 0) MUST be found when
  //     aborting after step 1 completes.
  //   - Key at offset 4196 (0x1064, in thread 0 of step 1) MUST NOT be scanned.
  unsigned char h_step0[20];
  std::string addr_step0;
  AddressOf("2DF4", h_step0, &addr_step0);  // 0x2000 + 3572 (step 0, thread 3)

  unsigned char h_step1[20];
  std::string addr_step1;
  AddressOf("3064", h_step1, &addr_step1);  // 0x2000 + 4196 (step 1, thread 0)

  std::vector<uint8_t> gpu_table(65536, 0);
  gpu_table[Prefix16(h_step0)] = 1;
  gpu_table[Prefix16(h_step1)] = 1;
  std::atomic<bool> stop{false};

  GpuEngineConfig cfg;
  cfg.grid_size_x = 1;
  cfg.grid_size_y = 4;  // 4 threads => step_keys = 4 * 1024 = 4096
  constexpr uint64_t kLastCompletedStep = 1;

  GpuEngineHooks hooks;
  hooks.on_step = [&](uint64_t steps_done, uint64_t /*keys_done*/) {
    if (steps_done == kLastCompletedStep) {
      stop.store(true);
    }
  };

  GpuEngine gpu(secp_, gpu_table, cfg, hooks);
  const uint64_t step_keys =
      static_cast<uint64_t>(gpu.nbThread()) * GpuEngine::kStepSize;
  ASSERT_EQ(step_keys, 4096u);

  Chunk chunk = ChunkAt(RK("2000"), step_keys * 4);
  EngineBatchResult res = gpu.scan(chunk, gpu_table, stop);

  EXPECT_TRUE(res.aborted_early);
  EXPECT_EQ(res.keys_scanned, kLastCompletedStep * step_keys);

  // Verify contiguous coverage of [0x2000, 0x2000 + 4096):
  // addr_step0 (at offset 3572) was found, while addr_step1 (at offset 4196) was not.
  EXPECT_NE(std::find(res.matched_addresses.begin(),
                      res.matched_addresses.end(), addr_step0),
            res.matched_addresses.end());
  EXPECT_EQ(std::find(res.matched_addresses.begin(),
                      res.matched_addresses.end(), addr_step1),
            res.matched_addresses.end());
}

TEST_F(GpuEngineTest, RepeatedConstructDestroyDoesNotLeakPinnedMemory) {
  std::vector<uint8_t> gpu_table(65536, 0);
  gpu_table[0x1234] = 1;

  GpuEngineConfig cfg;
  cfg.grid_size_x = 4;
  cfg.grid_size_y = 64;

  for (int i = 0; i < 50; ++i) {
    ASSERT_NO_THROW({
      GpuEngine gpu(secp_, gpu_table, cfg);
      EXPECT_EQ(gpu.nbThread(), 256);
    });
  }
}

TEST_F(GpuEngineTest, StopFlagSetBeforeScanReturnsZeroKeys) {
  std::vector<uint8_t> gpu_table(65536, 0);
  GpuEngine gpu(secp_, gpu_table);
  std::atomic<bool> stop{true};

  Chunk chunk = ChunkAt(RK("1000"), 4096);
  EngineBatchResult res = gpu.scan(chunk, gpu_table, stop);

  EXPECT_TRUE(res.aborted_early);
  EXPECT_EQ(res.keys_scanned, 0u);
  EXPECT_TRUE(res.matched_addresses.empty());
}

TEST_F(GpuEngineTest, SolverFindsRealFullMatchWithGpuEngine) {
  TempDir temp_dir;
  auto log_path = temp_dir.path() / "scanned_ranges.jsonl";
  auto match_path = temp_dir.path() / "found_matches.jsonl";

  unsigned char h160[20];
  std::string target_addr;
  AddressOf("ABCD", h160, &target_addr);

  Int start_int;
  start_int.SetBase16(const_cast<char*>("ABCD"));
  start_int.Sub(100);

  GeneratorConfig chunk_cfg;
  chunk_cfg.range_start = RangeKey(start_int);
  chunk_cfg.range_end = chunk_cfg.range_start.plus(4095);
  chunk_cfg.chunk_size = 4096;
  chunk_cfg.gpu_stride = 4096;
  chunk_cfg.seed = 42;
  auto gen = std::make_shared<RandomChunkGenerator>(chunk_cfg);

  LoggerConfig log_cfg;
  log_cfg.directory = temp_dir.path();
  log_cfg.flush_every_n = 1;
  auto logger = std::make_shared<JsonRangeLogger>(log_cfg);

  auto matcher =
      std::make_shared<AddressMatcher>(std::vector<std::string>{}, target_addr);

  std::atomic<bool> stop_flag{false};
  MatchWriterConfig match_cfg;
  match_cfg.output_path = match_path;
  match_cfg.stop_flag = &stop_flag;
  auto writer = std::make_shared<FoundMatchWriter>(match_cfg);

  std::vector<uint8_t> gpu_table(65536, 0);
  gpu_table[Prefix16(h160)] = 1;

  auto engine = std::make_shared<GpuEngine>(secp_, gpu_table);

  SolverConfig solver_cfg;
  solver_cfg.gpu_table = gpu_table;
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
}

#endif  // PUZZLE71_ENABLE_CUDA

