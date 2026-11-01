#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "RandomChunkGenerator.h"

namespace puzzle {

// Result of scanning a single Chunk via IEngine::scan().
//
// DENSE GPU_TABLE ASYMMETRY NOTE:
// CpuEngine accepts arbitrary gpu_table density (including all-ones / empty
// filter) without candidate truncation because matches are appended directly
// to host vectors. GpuEngine is bounded by VanitySearch's per-step device
// output buffer (maxFound, GPUEngine.cu:210,576) and therefore rejects dense
// gpu_tables (popcount > GpuEngine::kMaxGpuTablePopcount) with
// std::runtime_error at construction/scan time to guarantee zero silent
// candidate truncation. Callers requiring dense or unfiltered scans must use
// CpuEngine or reduce the number of active prefixes.
struct EngineBatchResult {
  std::vector<std::array<uint8_t, 20>> matched_hash160s;   // hits from this batch
  std::vector<std::string>             matched_addresses;  // already Base58-encoded
  // matched_privkeys_hex[i] corresponds to matched_addresses[i] for ALL matches
  // (the engine populates this for every hit without knowing classification).
  // The solver MUST NOT pass it to recordMatch for prefix matches —
  // FoundMatchWriter strips it defensively, but the solver should not set it in the first place.
  std::vector<std::string>             matched_privkeys_hex;
  uint64_t keys_scanned = 0;
  // True if the engine stopped scanning before completing the chunk due to stop_flag.
  bool aborted_early = false;
};

class IEngine {
 public:
  virtual ~IEngine() = default;

  // Scan a chunk and return any hash160s whose first 2 bytes hit gpu_table[idx] != 0.
  // STOP_FLAG CONTRACT: The engine should periodically poll stop_flag during batch
  // execution and return early (with aborted_early=true) if stop_flag is raised.
  // The host-side matcher is invoked by the solver on returned matches.
  virtual EngineBatchResult scan(const Chunk& chunk,
                                 const std::vector<uint8_t>& gpu_table,
                                 std::atomic<bool>& stop_flag) = 0;

  // LIVE PROGRESS CONTRACT:
  // When reportsLiveProgress() returns true, the engine MUST fetch_add(1)
  // on the counter set via setProgressCounter() for every key it fully
  // scans. It MUST NOT increment the counter before a key is hashed; the
  // counter therefore under-approximates the scan by at most one key at
  // any instant. The engine MUST leave the counter untouched after
  // setProgressCounter(nullptr) is called. This is what makes GUI 60fps
  // telemetry accurate without requiring any lock or callback.
  //
  // Optional live-progress support. Default impl returns false / no-ops
  // so MockEngine and any test double continue to work unchanged.
  // When reportsLiveProgress() returns true, the caller (PuzzleSolver)
  // wires its own atomic counter into setProgressCounter() and the
  // engine MUST fetch_add(1) on that counter for every key (or for
  // every batch, with accuracy rounded down to the batch size) that it
  // has fully scanned. The caller will NOT also add result.keys_scanned
  // on scan() return — the engine's per-key increments are the sole
  // source of keys_scanned during the run.
  virtual bool reportsLiveProgress() const noexcept { return false; }
  virtual void setProgressCounter(std::atomic<uint64_t>* /*counter*/) {}
};

}  // namespace puzzle
