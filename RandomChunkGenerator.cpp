#include "RandomChunkGenerator.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace puzzle {

RandomChunkGenerator::RandomChunkGenerator(const GeneratorConfig& config)
    : config_(config), rng_(config.seed) {
  if (config_.gpu_stride == 0) {
    throw std::invalid_argument("RandomChunkGenerator: gpu_stride cannot be 0");
  }

  // Round chunk_size down to the nearest multiple of gpu_stride
  effective_chunk_keys_ = (config_.chunk_size / config_.gpu_stride) * config_.gpu_stride;
  if (effective_chunk_keys_ == 0) {
    throw std::invalid_argument("RandomChunkGenerator: chunk_size rounds down to 0 keys");
  }

  if (config_.range_start > config_.range_end) {
    throw std::invalid_argument("RandomChunkGenerator: range_start > range_end");
  }

  // Compute total keys in [range_start, range_end]
  total_keys_ = config_.range_start.distanceTo(config_.range_end);
  total_keys_.AddOne();

  Int min_required(effective_chunk_keys_);
  if (total_keys_.IsLower(&min_required)) {
    throw std::invalid_argument("RandomChunkGenerator: range is smaller than effective chunk size");
  }
}

std::optional<Chunk> RandomChunkGenerator::next() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Max valid start is range_end - effective_chunk_keys_ + 1
  Int max_s_val(const_cast<Int*>(&config_.range_end.value));
  max_s_val.Sub(effective_chunk_keys_ - 1);
  RangeKey max_start(max_s_val);

  RangeKey chosen_start;
  bool found_random = false;
  uint32_t collisions = 0;

  // Try random placements up to max_collision_retries collisions
  while (collisions <= config_.max_collision_retries) {
    RangeKey candidate = drawRandomStartLocked(max_start);
    if (!overlapsLocked(candidate, effective_chunk_keys_)) {
      chosen_start = candidate;
      found_random = true;
      break;
    }
    collisions++;
  }

  // Fallback: switch to deterministic gap scan if random placement collided too many times
  if (!found_random) {
    auto gap = findFirstGapLocked(effective_chunk_keys_);
    if (!gap.has_value()) {
      return std::nullopt;  // Range exhausted: no free contiguous gap >= effective_chunk_keys_
    }
    chosen_start = *gap;
    gap_fallback_hits_++;
  } else {
    random_hits_++;
  }

  chunks_issued_++;
  RangeKey chosen_end = chosen_start.plus(effective_chunk_keys_ - 1);
  addIntervalLocked(chosen_start, chosen_end);

  Chunk c;
  c.start = chosen_start;
  c.end = chosen_end;
  c.requested_keys = config_.chunk_size;
  c.scanned_keys = effective_chunk_keys_;
  return c;
}

void RandomChunkGenerator::markScanned(const RangeKey& start, uint64_t keys) {
  if (keys == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);

  RangeKey end = start.plus(keys - 1);
  // Out of bounds check
  if (end < config_.range_start || start > config_.range_end) {
    return;
  }

  // Clip to [range_start, range_end]
  RangeKey s = (start < config_.range_start) ? config_.range_start : start;
  RangeKey e = (end > config_.range_end) ? config_.range_end : end;

  addIntervalLocked(s, e);
}

GeneratorStats RandomChunkGenerator::getStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  GeneratorStats s;
  s.chunks_issued = chunks_issued_;
  s.random_hits = random_hits_;
  s.gap_fallback_hits = gap_fallback_hits_;

  // Sum lengths of all disjoint merged intervals
  Int scanned_total(uint64_t(0));
  for (const auto& [start, end] : scanned_intervals_) {
    Int len = start.distanceTo(end);
    len.AddOne();
    scanned_total.Add(&len);
  }

  // keys_remaining = total_keys_ - scanned_total
  Int remaining;
  Int tot(const_cast<Int*>(&total_keys_));
  remaining.Sub(&tot, &scanned_total);
  s.keys_remaining = remaining;

  return s;
}

// ------------------------------------------------------------- private helpers

void RandomChunkGenerator::addIntervalLocked(const RangeKey& s, const RangeKey& e) {
  RangeKey new_s = s;
  RangeKey new_e = e;

  // Find candidate intervals that touch or overlap [new_s, new_e]
  auto it = scanned_intervals_.upper_bound(new_s);
  if (it != scanned_intervals_.begin()) {
    auto prev = std::prev(it);
    // Touch condition: prev->second + 1 >= new_s
    if (prev->second.plus(1) >= new_s) {
      new_s = (prev->first < new_s) ? prev->first : new_s;
      new_e = (prev->second > new_e) ? prev->second : new_e;
      it = prev;
    }
  }

  // Merge subsequent overlapping or adjacent intervals
  while (it != scanned_intervals_.end()) {
    if (it->first <= new_e.plus(1)) {
      new_e = (it->second > new_e) ? it->second : new_e;
      it = scanned_intervals_.erase(it);
    } else {
      break;
    }
  }

  scanned_intervals_[new_s] = new_e;
}

std::optional<RangeKey> RandomChunkGenerator::findFirstGapLocked(uint64_t needed_keys) const {
  RangeKey cursor = config_.range_start;

  for (const auto& [start, end] : scanned_intervals_) {
    if (start > cursor) {
      // Gap exists between cursor and start - 1
      RangeKey needed_end = cursor.plus(needed_keys - 1);
      if (needed_end < start && needed_end <= config_.range_end) {
        return cursor;
      }
    }
    if (end >= config_.range_end) {
      return std::nullopt;
    }
    cursor = end.plus(1);
  }

  // Check tail gap after last interval up to range_end
  if (cursor <= config_.range_end) {
    RangeKey needed_end = cursor.plus(needed_keys - 1);
    if (needed_end <= config_.range_end) {
      return cursor;
    }
  }

  return std::nullopt;
}

bool RandomChunkGenerator::overlapsLocked(const RangeKey& s, uint64_t len) const {
  if (scanned_intervals_.empty()) return false;

  auto it = scanned_intervals_.upper_bound(s);
  // Check preceding interval: if prev->end >= s, it contains or overlaps s
  if (it != scanned_intervals_.begin()) {
    auto prev = std::prev(it);
    if (prev->second >= s) {
      return true;
    }
  }

  // Check following interval: if it->start <= s + len - 1, it overlaps the candidate tail
  if (it != scanned_intervals_.end()) {
    RangeKey s_end = s.plus(len - 1);
    if (it->first <= s_end) {
      return true;
    }
  }

  return false;
}

RangeKey RandomChunkGenerator::drawRandomStartLocked(const RangeKey& max_start) {
  // Number of candidate start points: span = max_start - range_start + 1
  Int span = config_.range_start.distanceTo(max_start);
  span.AddOne();

  // Rejection sampling without modulo:
  // TODO(future-perf): for spans whose top-limb value is a power of two
  // (e.g., 2^70 - epsilon for puzzle #71), the current mask accepts only
  // ~50% of draws. A tighter mask of (bit_length - 1) bits would eliminate
  // the rejection at the cost of one extra edge-case check. Not done now
  // because chunk generation is O(1) per GPU scan and the GPU dominates.
  // Determine highest non-zero limb of span.
  int high_limb = NB64BLOCK - 1;
  while (high_limb > 0 && span.bits64[high_limb] == 0) {
    high_limb--;
  }

  // Find exact bit length of the top limb to mask unused higher bits
  uint64_t top_val = span.bits64[high_limb];
  int top_bits = 0;
  while ((1ULL << top_bits) <= top_val && top_bits < 64) {
    top_bits++;
    if (top_bits == 64) break;
  }
  uint64_t top_mask = (top_bits == 64) ? ~0ULL : ((1ULL << top_bits) - 1);

  Int cand;
  while (true) {
    for (int i = 0; i <= high_limb; ++i) {
      cand.bits64[i] = rng_();
    }
    cand.bits64[high_limb] &= top_mask;
    for (int i = high_limb + 1; i < NB64BLOCK; ++i) {
      cand.bits64[i] = 0;
    }

    // Rejection check: accept strictly if cand < span
    if (cand.IsLower(&span)) {
      break;
    }
  }

  // Return range_start + cand
  Int start_val;
  Int base(const_cast<Int*>(&config_.range_start.value));
  start_val.Add(&base, &cand);
  return RangeKey(start_val);
}

}  // namespace puzzle
