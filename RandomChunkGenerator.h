#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <vector>

#include "RangeKey.h"

namespace puzzle {

struct GeneratorConfig {
  RangeKey range_start;               // inclusive lower bound
  RangeKey range_end;                 // inclusive upper bound
  uint64_t chunk_size = 100'000'000;  // requested keys per chunk
  uint64_t gpu_stride = 262144;       // keys per GPU batch (gridSizeX * gridSizeY * blockSize)
  uint32_t max_collision_retries = 8; // random collision attempts before gap-scan fallback
  uint64_t seed = 1;                  // independent 64-bit seed (does not alter global rndl())
};

struct Chunk {
  RangeKey start;                     // inclusive start
  RangeKey end;                       // inclusive end == start + scanned_keys - 1
  uint64_t requested_keys = 0;        // == config.chunk_size
  uint64_t scanned_keys = 0;          // chunk_size rounded DOWN to nearest multiple of gpu_stride
};

struct GeneratorStats {
  uint64_t chunks_issued = 0;
  uint64_t random_hits = 0;
  uint64_t gap_fallback_hits = 0;
  Int keys_remaining;                 // unscanned keys remaining (2^70 > uint64_t)
};

class RandomChunkGenerator {
 public:
  explicit RandomChunkGenerator(const GeneratorConfig& config);

  // Thread-safe: returns next disjoint chunk or std::nullopt when no gap >= scanned_keys exists.
  std::optional<Chunk> next();

  // Mark an already-scanned interval [start, start + keys - 1] (e.g. on resume).
  // Idempotent and handles arbitrary overlaps.
  void markScanned(const RangeKey& start, uint64_t keys);

  // Snapshot of generator progress & fallback telemetry.
  GeneratorStats getStats() const;

 private:
  GeneratorConfig config_;
  uint64_t effective_chunk_keys_;      // chunk_size rounded down to multiple of gpu_stride
  Int total_keys_;                     // range_end - range_start + 1

  mutable std::mutex mutex_;
  std::mt19937_64 rng_;

  // Set of disjoint scanned intervals: maps interval_start -> interval_end (inclusive)
  std::map<RangeKey, RangeKey> scanned_intervals_;

  uint64_t chunks_issued_ = 0;
  uint64_t random_hits_ = 0;
  uint64_t gap_fallback_hits_ = 0;

  // Internal helper to insert an interval [s, e] into scanned_intervals_ merging overlaps
  void addIntervalLocked(const RangeKey& s, const RangeKey& e);

  // Internal helper to find first free gap of size >= effective_chunk_keys_
  std::optional<RangeKey> findFirstGapLocked(uint64_t needed_keys) const;

  // Draw uniform random key in [range_start, max_start]
  RangeKey drawRandomStartLocked(const RangeKey& max_start);

  // Checks whether [s, s + len - 1] overlaps any existing interval
  bool overlapsLocked(const RangeKey& s, uint64_t len) const;
};

}  // namespace puzzle
