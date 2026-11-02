#ifndef PUZZLE71_GUI_CONTROLLER_H_
#define PUZZLE71_GUI_CONTROLLER_H_

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cli.h"  // for CliOptions, SolverContext, RunResult, SolverStats

namespace puzzle::gui {

enum class GuiState { Idle, Running, Stopped, Found, Exhausted, Error };

struct GuiStats {
  GuiState    state             = GuiState::Idle;
  uint64_t    chunks_scanned    = 0;
  uint64_t    keys_scanned      = 0;
  double      elapsed_seconds   = 0.0;
  double      rate_keys_per_sec = 0.0;
  uint64_t    matches_found     = 0;
  std::string last_error;
  std::string current_range_start;
  std::string current_range_end;
  double      current_chunk_progress = 0.0;
  uint64_t    total_keyspace    = 0;
  std::string total_keyspace_str;
  double      progress_percent  = 0.0;
};

class GuiController {
 public:
  // CONTRACT: throws std::invalid_argument if options are self-inconsistent:
  //   - range_start > range_end
  //   - chunk_size == 0 or chunk_size < gpu_stride
  //   - engine not in {"cpu", "gpu"}
  // Validation is cheap (no secp->Init). The heavy SolverContext is
  // built lazily inside start() so re-opening a GUI with a bad range
  // does not pay a 200ms secp init.
  explicit GuiController(puzzle::cli::CliOptions options);

  // Destructor: requestStop() then wait(). Never throws.
  ~GuiController();

  GuiController(const GuiController&) = delete;
  GuiController& operator=(const GuiController&) = delete;

  // START CONTRACT: single-shot. The first successful start() spawns a
  // worker thread that calls RunContext(*ctx_, nullptr). Every subsequent
  // call returns false — regardless of state (Running/Stopped/Found/
  // Exhausted/Error). To restart, destroy and reconstruct.
  // Uses std::atomic<bool> started_{false}; NOT `state == Running`.
  bool start();

  // No-op if !started_ or if the worker already exited (Idle/terminal).
  // Sets stop_flag_ = true; does NOT block. If the run already ended in
  // Found/Exhausted, this MUST NOT overwrite the state.
  void requestStop();

  // Idempotent. First call joins the worker if it was spawned. Subsequent
  // calls return immediately. If !started_, returns immediately.
  void wait();

  // Non-blocking, thread-safe, called at 60fps from the render loop.
  // Reads solver->getStats() under a mutex (or via atomic snapshot).
  // LIFETIME CONTRACT: getStats() must not run concurrently with
  // ~GuiController(). Intended GUI usage keeps the controller alive across
  // the entire render loop, so this holds naturally. Do NOT spawn a thread
  // that destroys the controller while another thread is calling getStats()
  // unless you join that thread first.
  GuiStats getStats() const;

  std::filesystem::path foundMatchesPath() const;
  std::filesystem::path scannedRangesPath() const;

  // Retrieve and clear any newly discovered prefix match addresses. Thread-safe.
  std::vector<std::string> popFoundPrefixes();

 private:
  puzzle::cli::CliOptions options_;
  std::unique_ptr<puzzle::cli::SolverContext> ctx_;   // built in start()

  std::atomic<bool>  started_{false};
  std::atomic<bool>  stop_requested_{false};
  std::thread        worker_;
  mutable std::mutex state_mu_;      // guards terminal_state_ / last_error_ / final_stats_
  GuiState           terminal_state_ = GuiState::Idle;
  std::string        last_error_;
  SolverStats        final_stats_{};
  bool               has_final_stats_ = false;

  mutable std::mutex       prefix_mu_;
  std::vector<std::string> found_prefixes_;
};

}  // namespace puzzle::gui

#endif  // PUZZLE71_GUI_CONTROLLER_H_

