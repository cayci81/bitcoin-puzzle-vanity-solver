#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Engine.h"
#include "FoundMatchWriter.h"
#include "JsonRangeLogger.h"
#include "PrefixFilter.h"
#include "RandomChunkGenerator.h"

namespace puzzle {

struct SolverStats {
  uint64_t chunks_scanned = 0;
  uint64_t keys_scanned = 0;
  // Total number of matches recorded (Full + Prefix). Unmatched candidate hashes
  // returned by the engine (classified as MatchKind::None) are not counted.
  uint64_t matches_found = 0;
  // Wall-clock elapsed seconds measured from the start of the first run() invocation.
  // If run() is called multiple times, timing continues from the initial start time.
  double elapsed_seconds = 0.0;
  double rate_keys_per_sec = 0.0;
  std::string current_range_start;
  std::string current_range_end;
  uint64_t total_keyspace = 0;
  std::string total_keyspace_str;
  double progress_percent = 0.0;
};

enum class SolverExitReason {
  Exhausted,
  FullMatchFound,
  StoppedExternal,
};

struct SolverConfig {
  std::vector<uint8_t> gpu_table;
  std::atomic<bool>* stop_flag = nullptr;
  // Worker ID assigned to this solver instance, logged as gpu_id in JsonRangeLogger
  // and FoundMatchWriter records.
  int worker_id = 0;
  uint64_t max_chunks = 0;  // 0 = unlimited, >0 = limit number of chunks scanned
  std::function<void(const std::string& address)> on_prefix_found = nullptr;
  uint64_t total_keyspace = 0;
  std::string total_keyspace_str;
  double total_keyspace_double = 0.0;
};

class PuzzleSolver {
 public:
  PuzzleSolver(std::shared_ptr<IEngine> engine,
               std::shared_ptr<RandomChunkGenerator> generator,
               std::shared_ptr<JsonRangeLogger> logger,
               std::shared_ptr<AddressMatcher> matcher,
               std::shared_ptr<FoundMatchWriter> writer,
               const SolverConfig& config);

  // Runs the solver loop until the generator is exhausted or stop_flag is set.
  // STOP_FLAG ENTRY CONTRACT: If stop_flag is already true prior to calling run(),
  // run() exits immediately with SolverExitReason::StoppedExternal with zero chunks scanned.
  // Returns the reason why the solver exited.
  SolverExitReason run();

  // Returns a telemetry snapshot. Thread-safe.
  SolverStats getStats() const;

 private:
  std::shared_ptr<IEngine> engine_;
  std::shared_ptr<RandomChunkGenerator> generator_;
  std::shared_ptr<JsonRangeLogger> logger_;
  std::shared_ptr<AddressMatcher> matcher_;
  std::shared_ptr<FoundMatchWriter> writer_;
  SolverConfig config_;

  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point end_time_;
  std::atomic<bool> started_{false};
  std::atomic<bool> finished_{false};
  std::atomic<uint64_t> chunks_scanned_{0};
  std::atomic<uint64_t> keys_scanned_{0};
  std::atomic<uint64_t> matches_found_{0};
  mutable std::mutex range_mu_;
  std::string current_range_start_;
  std::string current_range_end_;
};

}  // namespace puzzle
