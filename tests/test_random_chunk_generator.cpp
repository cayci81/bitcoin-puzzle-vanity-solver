// Unit tests for puzzle::RandomChunkGenerator (TDD - no implementation exists yet).
//
// API contract exercised here (header "RandomChunkGenerator.h"):
//
//   struct GeneratorConfig {
//     RangeKey  range_start;               // inclusive
//     RangeKey  range_end;                 // inclusive
//     uint64_t  chunk_size = 100'000'000;  // requested keys per chunk
//     uint64_t  gpu_stride;                // gridSizeX * gridSizeY * blockSize (keys per kernel step)
//     uint32_t  max_collision_retries = 8; // random attempts that may collide before gap-scan
//     uint64_t  seed;                      // own std::mt19937_64; does NOT touch the global rndl()
//   };
//   struct Chunk {
//     RangeKey start;                      // inclusive
//     RangeKey end;                        // inclusive == start + scanned_keys - 1
//     uint64_t requested_keys;             // == config.chunk_size
//     uint64_t scanned_keys;               // chunk_size rounded DOWN to a multiple of gpu_stride
//   };
//   struct GeneratorStats {
//     uint64_t chunks_issued, random_hits, gap_fallback_hits;
//     Int      keys_remaining;             // unscanned keys left in the range (Int: 2^70 > uint64)
//   };
//   class RandomChunkGenerator {
//     explicit RandomChunkGenerator(const GeneratorConfig&);   // throws std::invalid_argument
//     std::optional<Chunk> next();         // thread-safe; nullopt == exhausted
//     void markScanned(const RangeKey& start, uint64_t keys);  // resume; idempotent, overlap-safe
//     GeneratorStats getStats() const;
//   };
//
// Semantics:
//  * next() draws a uniform random start in [range_start, range_end - scanned_keys + 1].
//    If the candidate overlaps anything issued/marked it counts as a collision. Once
//    the number of collisions in a single next() call exceeds `max_collision_retries`,
//    that SAME call switches to deterministic gap-scanning (lowest free gap >= scanned_keys).
//    (retries = N  => at most N+1 random draws per call; retries = 0 => 1 draw.)
//  * "random_hits" = chunks obtained by random placement, "gap_fallback_hits" = chunks
//    obtained by gap scan. random_hits + gap_fallback_hits == chunks_issued.
//  * With max_collision_retries == 0 the first collision triggers the fallback.
//  * nullopt  <=>  no contiguous free gap of >= scanned_keys exists.
//  * Chunk start is NOT stride-aligned (pure uniform sampling); only the length is rounded.

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "RandomChunkGenerator.h"
#include "test_support.h"

using namespace puzzle;
using namespace puzzle::test;

namespace {

const char* const kPuzzleStart = "400000000000000000";
const char* const kPuzzleEnd = "7fffffffffffffffff";
constexpr uint64_t kStride = 262144;       // 2^18, typical GPU step
constexpr uint64_t kChunk100M = 100'000'000;
constexpr uint64_t kScanned100M = 381 * kStride;  // floor(1e8 / 2^18) * 2^18 = 99'876'864

GeneratorConfig PuzzleConfig(uint64_t seed = 1, uint32_t retries = 8) {
  GeneratorConfig c;
  c.range_start = RK(kPuzzleStart);
  c.range_end = RK(kPuzzleEnd);
  c.chunk_size = kChunk100M;
  c.gpu_stride = kStride;
  c.max_collision_retries = retries;
  c.seed = seed;
  return c;
}

// Tiny range of `total_keys` keys starting at 0x100000 for exhaustive tests.
GeneratorConfig SmallConfig(uint64_t total_keys, uint64_t chunk, uint64_t stride,
                            uint32_t retries, uint64_t seed) {
  GeneratorConfig c;
  c.range_start = RK("100000");
  c.range_end = c.range_start.plus(total_keys - 1);
  c.chunk_size = chunk;
  c.gpu_stride = stride;
  c.max_collision_retries = retries;
  c.seed = seed;
  return c;
}

std::vector<Chunk> Drain(RandomChunkGenerator& gen, size_t limit = 1'000'000) {
  std::vector<Chunk> out;
  while (out.size() < limit) {
    auto c = gen.next();
    if (!c) break;
    out.push_back(*c);
  }
  return out;
}

std::vector<Chunk> Take(RandomChunkGenerator& gen, size_t n) {
  std::vector<Chunk> out;
  for (size_t i = 0; i < n; ++i) {
    auto c = gen.next();
    if (!c) break;
    out.push_back(*c);
  }
  return out;
}

void ExpectPairwiseDisjoint(std::vector<Chunk> chunks) {
  std::sort(chunks.begin(), chunks.end(),
            [](const Chunk& a, const Chunk& b) { return a.start < b.start; });
  for (size_t i = 1; i < chunks.size(); ++i) {
    ASSERT_TRUE(chunks[i - 1].end < chunks[i].start)
        << "overlap between [" << chunks[i - 1].start.ToHex() << ","
        << chunks[i - 1].end.ToHex() << "] and [" << chunks[i].start.ToHex() << ","
        << chunks[i].end.ToHex() << "]";
  }
}

// Inclusive [lo, hi] offsets (relative to `base`) of the given chunks, sorted.
std::vector<std::pair<uint64_t, uint64_t>> Intervals(const RangeKey& base,
                                                    const std::vector<Chunk>& chunks) {
  std::vector<std::pair<uint64_t, uint64_t>> v;
  for (const auto& c : chunks) v.emplace_back(Offset(base, c.start), Offset(base, c.end));
  std::sort(v.begin(), v.end());
  return v;
}

// Largest contiguous free gap in [0, total) given sorted, disjoint intervals.
uint64_t MaxFreeGap(uint64_t total, const std::vector<std::pair<uint64_t, uint64_t>>& iv) {
  uint64_t best = 0, cursor = 0;
  for (const auto& [lo, hi] : iv) {
    if (lo > cursor) best = std::max(best, lo - cursor);
    cursor = hi + 1;
  }
  if (total > cursor) best = std::max(best, total - cursor);
  return best;
}

}  // namespace

// ---------------------------------------------------------------- construction

TEST(RandomChunkGeneratorConfig, RoundsChunkDownToStrideMultiple) {
  RandomChunkGenerator gen(PuzzleConfig());
  auto c = gen.next();
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->requested_keys, kChunk100M);
  EXPECT_EQ(c->scanned_keys, kScanned100M);
  EXPECT_EQ(c->scanned_keys % kStride, 0u);
  EXPECT_EQ(c->scanned_keys, 99'876'864u);
  EXPECT_TRUE(c->end == c->start.plus(c->scanned_keys - 1));
}

TEST(RandomChunkGeneratorConfig, ExactMultipleKeepsRequestedEqualToScanned) {
  auto cfg = PuzzleConfig();
  cfg.chunk_size = 4 * kStride;
  RandomChunkGenerator gen(cfg);
  auto c = gen.next();
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->requested_keys, 4 * kStride);
  EXPECT_EQ(c->scanned_keys, 4 * kStride);
}

TEST(RandomChunkGeneratorConfig, RejectsInvalidConfiguration) {
  auto zero_stride = PuzzleConfig();
  zero_stride.gpu_stride = 0;
  EXPECT_THROW(RandomChunkGenerator{zero_stride}, std::invalid_argument);

  auto chunk_below_stride = PuzzleConfig();
  chunk_below_stride.chunk_size = kStride - 1;  // rounds down to 0 keys
  EXPECT_THROW(RandomChunkGenerator{chunk_below_stride}, std::invalid_argument);

  auto inverted = PuzzleConfig();
  std::swap(inverted.range_start, inverted.range_end);
  EXPECT_THROW(RandomChunkGenerator{inverted}, std::invalid_argument);

  auto range_too_small = SmallConfig(/*total*/ 500, /*chunk*/ 1000, /*stride*/ 1000, 8, 1);
  EXPECT_THROW(RandomChunkGenerator{range_too_small}, std::invalid_argument);
}

TEST(RandomChunkGeneratorConfig, RangeExactlyOneChunkWideYieldsWholeRangeThenExhausts) {
  RandomChunkGenerator gen(SmallConfig(1000, 1000, 1000, 8, 7));
  auto c = gen.next();
  ASSERT_TRUE(c.has_value());
  EXPECT_TRUE(c->start == RK("100000"));
  EXPECT_TRUE(c->end == RK("100000").plus(999));
  EXPECT_FALSE(gen.next().has_value());
}

// -------------------------------------------------------------------- sampling

TEST(RandomChunkGeneratorSampling, ChunksStayInsideMainRange) {
  RandomChunkGenerator gen(PuzzleConfig(42));
  const RangeKey lo = RK(kPuzzleStart), hi = RK(kPuzzleEnd);
  for (const auto& c : Take(gen, 500)) {
    EXPECT_TRUE(lo <= c.start);
    EXPECT_TRUE(c.end <= hi);
    EXPECT_TRUE(c.start <= c.end);
  }
}

TEST(RandomChunkGeneratorSampling, SameSeedReproducesSequence) {
  RandomChunkGenerator a(PuzzleConfig(1234)), b(PuzzleConfig(1234));
  auto ca = Take(a, 100), cb = Take(b, 100);
  ASSERT_EQ(ca.size(), cb.size());
  for (size_t i = 0; i < ca.size(); ++i) EXPECT_TRUE(ca[i].start == cb[i].start) << i;
}

TEST(RandomChunkGeneratorSampling, DifferentSeedsDiverge) {
  RandomChunkGenerator a(PuzzleConfig(1)), b(PuzzleConfig(2));
  auto ca = Take(a, 20), cb = Take(b, 20);
  size_t same = 0;
  for (size_t i = 0; i < ca.size(); ++i) same += (ca[i].start == cb[i].start);
  EXPECT_EQ(same, 0u);
}

TEST(RandomChunkGeneratorSampling, DoesNotConsumeGlobalRandomState) {
  // The generator owns its RNG; VanitySearch's global rndl() must stay untouched
  // so existing key-generation behaviour is not perturbed.
  rseed(99);
  unsigned long expected = rndl();
  rseed(99);
  RandomChunkGenerator gen(PuzzleConfig(5));
  Take(gen, 10);
  EXPECT_EQ(rndl(), expected);
}

TEST(RandomChunkGeneratorSampling, StartsCoverWholeRangeNotJustTheBottom) {
  // Smoke test for uniformity: 4000 draws into 8 equal buckets; each expects ~500.
  RandomChunkGenerator gen(PuzzleConfig(2024));
  int hist[8] = {};
  const RangeKey lo = RK(kPuzzleStart);
  for (const auto& c : Take(gen, 4000)) ++hist[Bucket8(lo, c.start)];
  for (int b = 0; b < 8; ++b) {
    EXPECT_GT(hist[b], 300) << "bucket " << b;
    EXPECT_LT(hist[b], 700) << "bucket " << b;
  }
}

TEST(RandomChunkGeneratorSampling, NoOverlapAcrossThousandChunksInPuzzleRange) {
  RandomChunkGenerator gen(PuzzleConfig(77));
  auto chunks = Take(gen, 1000);
  ASSERT_EQ(chunks.size(), 1000u);
  ExpectPairwiseDisjoint(chunks);
}

// ------------------------------------------------------------- stats & fallback

TEST(RandomChunkGeneratorStats, HugeRangeNeverNeedsFallback) {
  RandomChunkGenerator gen(PuzzleConfig(11));
  Take(gen, 1000);
  auto s = gen.getStats();
  EXPECT_EQ(s.chunks_issued, 1000u);
  EXPECT_EQ(s.random_hits, 1000u);
  EXPECT_EQ(s.gap_fallback_hits, 0u);
}

TEST(RandomChunkGeneratorStats, KeysRemainingTracksIssuedKeys) {
  const uint64_t total = 10'000'000, chunk = 1000;
  RandomChunkGenerator gen(SmallConfig(total, chunk, chunk, 8, 3));
  EXPECT_EQ(U64(gen.getStats().keys_remaining), total);
  for (uint64_t k = 1; k <= 25; ++k) {
    ASSERT_TRUE(gen.next().has_value());
    EXPECT_EQ(U64(gen.getStats().keys_remaining), total - k * chunk);
  }
}

TEST(RandomChunkGeneratorStats, RandomPlusGapHitsEqualChunksIssued) {
  RandomChunkGenerator gen(SmallConfig(100'000, 1000, 1000, 2, 5));
  Drain(gen);
  auto s = gen.getStats();
  EXPECT_EQ(s.random_hits + s.gap_fallback_hits, s.chunks_issued);
}

TEST(RandomChunkGeneratorFallback, ZeroRetriesTriggersGapScanWhenRangeIsCrowded) {
  RandomChunkGenerator gen(SmallConfig(100'000, 1000, 1000, /*retries*/ 0, 9));
  auto chunks = Drain(gen);
  auto s = gen.getStats();
  EXPECT_GT(s.gap_fallback_hits, 0u) << "fallback never triggered";
  EXPECT_EQ(s.chunks_issued, chunks.size());
  ExpectPairwiseDisjoint(chunks);
}

TEST(RandomChunkGeneratorFallback, MoreRetriesMeansFewerFallbacks) {
  // Summed over 20 seeds so the comparison is statistically robust.
  uint64_t fb_low = 0, fb_high = 0;
  for (uint64_t seed = 1; seed <= 20; ++seed) {
    RandomChunkGenerator low(SmallConfig(100'000, 1000, 1000, 0, seed));
    RandomChunkGenerator high(SmallConfig(100'000, 1000, 1000, 64, seed));
    Drain(low);
    Drain(high);
    fb_low += low.getStats().gap_fallback_hits;
    fb_high += high.getStats().gap_fallback_hits;
  }
  EXPECT_LT(fb_high, fb_low);
}

TEST(RandomChunkGeneratorFallback, GapScanFillsExactlyTheOnlyFreeGap) {
  // total = 1,000,000 keys; the only free gap is exactly one chunk wide (see offsets below).
  RandomChunkGenerator gen(SmallConfig(1'000'000, 1000, 1000, /*retries*/ 0, 13));
  const RangeKey base = RK("100000");
  gen.markScanned(base, 499'000);                 // offsets [0, 498999]
  gen.markScanned(base.plus(500'000), 500'000);   // offsets [500000, 999999]
  // Free gap: offsets [499000, 499999] -> exactly 1000 keys.
  auto c = gen.next();
  ASSERT_TRUE(c.has_value());
  EXPECT_TRUE(c->start == base.plus(499'000));
  EXPECT_TRUE(c->end == base.plus(499'999));
  EXPECT_EQ(gen.getStats().gap_fallback_hits, 1u);  // random hit odds ~1e-6 for this seed
  EXPECT_EQ(U64(gen.getStats().keys_remaining), 0u);
  EXPECT_FALSE(gen.next().has_value());
}

TEST(RandomChunkGeneratorFallback, GapSmallerThanChunkIsNeverUsed) {
  RandomChunkGenerator gen(SmallConfig(1'000'000, 1000, 1000, 0, 21));
  const RangeKey base = RK("100000");
  gen.markScanned(base, 499'500);                // [0, 499499]
  gen.markScanned(base.plus(500'000), 500'000);  // [500000, 999999]  -> gap of 500 keys only
  EXPECT_FALSE(gen.next().has_value());
}

// ------------------------------------------------------------------ exhaustion

TEST(RandomChunkGeneratorExhaustion, DrainLeavesNoGapLargeEnoughForAChunk) {
  const uint64_t total = 100'000, chunk = 1000;
  for (uint32_t retries : {0u, 8u}) {
    RandomChunkGenerator gen(SmallConfig(total, chunk, chunk, retries, 31));
    auto chunks = Drain(gen);
    ExpectPairwiseDisjoint(chunks);
    auto iv = Intervals(RK("100000"), chunks);
    EXPECT_LT(MaxFreeGap(total, iv), chunk) << "retries=" << retries;
    EXPECT_FALSE(gen.next().has_value());
    EXPECT_FALSE(gen.next().has_value()) << "exhaustion must be sticky";
    auto s = gen.getStats();
    EXPECT_EQ(s.chunks_issued, chunks.size());
    EXPECT_EQ(U64(s.keys_remaining), total - chunks.size() * chunk);
  }
}

TEST(RandomChunkGeneratorExhaustion, ChunkCountAtExhaustionIsWithinPackingBounds) {
  // 100 slots of 1000 keys. At exhaustion every gap is < 1000 keys, so each chunk
  // is followed by < 1000 wasted keys => at least 100000 / 2000 = 50 chunks. Max is 100.
  RandomChunkGenerator gen(SmallConfig(100'000, 1000, 1000, 0, 17));
  auto chunks = Drain(gen);
  EXPECT_GE(chunks.size(), 50u);
  EXPECT_LE(chunks.size(), 100u);
}

// ---------------------------------------------------------------------- resume

TEST(RandomChunkGeneratorResume, MarkedRangesAreNeverIssued) {
  const uint64_t total = 100'000, chunk = 1000;
  RandomChunkGenerator gen(SmallConfig(total, chunk, chunk, 8, 4));
  const RangeKey base = RK("100000");
  gen.markScanned(base, 50'000);  // lower half already done
  auto chunks = Drain(gen);
  ASSERT_FALSE(chunks.empty());
  for (const auto& c : chunks) EXPECT_GE(Offset(base, c.start), 50'000u);
  EXPECT_LE(chunks.size(), 50u);
}

TEST(RandomChunkGeneratorResume, MarkScannedIsIdempotentAndOverlapSafe) {
  const uint64_t total = 100'000;
  RandomChunkGenerator gen(SmallConfig(total, 1000, 1000, 8, 6));
  const RangeKey base = RK("100000");
  gen.markScanned(base, 10'000);
  gen.markScanned(base, 10'000);           // exact duplicate
  gen.markScanned(base.plus(5'000), 10'000);  // overlaps previous -> union [0, 14999]
  EXPECT_EQ(U64(gen.getStats().keys_remaining), total - 15'000);
}

// ----------------------------------------------------------------- concurrency

TEST(RandomChunkGeneratorConcurrency, ParallelNextProducesDisjointChunks) {
  RandomChunkGenerator gen(PuzzleConfig(555));
  std::vector<Chunk> all;
  std::mutex m;
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t) {
    workers.emplace_back([&] {
      auto mine = Take(gen, 200);
      std::lock_guard<std::mutex> lk(m);
      all.insert(all.end(), mine.begin(), mine.end());
    });
  }
  for (auto& w : workers) w.join();
  EXPECT_EQ(all.size(), 800u);
  ExpectPairwiseDisjoint(all);
  EXPECT_EQ(gen.getStats().chunks_issued, 800u);
}
