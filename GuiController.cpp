#include "GuiController.h"

#include <stdexcept>
#include <utility>

namespace puzzle::gui {

using puzzle::cli::CliOptions;
using puzzle::cli::RunContext;
using puzzle::cli::RunResult;
using puzzle::cli::SolverContext;

GuiController::GuiController(CliOptions options)
    : options_(std::move(options)) {
  if (options_.range_start > options_.range_end) {
    throw std::invalid_argument("range_start > range_end");
  }
  if (options_.chunk_size == 0 || options_.chunk_size < options_.gpu_stride) {
    throw std::invalid_argument("chunk_size must be >= gpu_stride and > 0");
  }
  if (options_.engine != "cpu" && options_.engine != "gpu") {
    throw std::invalid_argument("engine must be 'cpu' or 'gpu'");
  }
}

GuiController::~GuiController() {
  requestStop();
  wait();
}

bool GuiController::start() {
  // Single-shot atomic gate: once start() is called once, every subsequent
  // call permanently returns false regardless of worker state.
  if (started_.exchange(true, std::memory_order_acq_rel)) {
    return false;
  }

  worker_ = std::thread([this] {
    try {
      auto local_ctx = std::make_unique<SolverContext>(options_);
      SolverContext* ctx_ptr = nullptr;
      {
        std::lock_guard<std::mutex> lk(state_mu_);
        ctx_ = std::move(local_ctx);
        if (stop_requested_.load(std::memory_order_acquire)) {
          ctx_->stop_flag.store(true, std::memory_order_release);
        }
        ctx_ptr = ctx_.get();
      }

      RunResult r = RunContext(*ctx_ptr, nullptr);
      std::lock_guard<std::mutex> lk(state_mu_);
      final_stats_ = r.stats;
      has_final_stats_ = true;
      terminal_state_ = (r.reason == SolverExitReason::FullMatchFound)
                            ? GuiState::Found
                            : (r.reason == SolverExitReason::Exhausted)
                                  ? GuiState::Exhausted
                                  : GuiState::Stopped;  // StoppedExternal
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lk(state_mu_);
      terminal_state_ = GuiState::Error;
      last_error_ = e.what();
    } catch (...) {
      std::lock_guard<std::mutex> lk(state_mu_);
      terminal_state_ = GuiState::Error;
      last_error_ = "Unknown non-standard exception in worker thread";
    }
  });

  return true;
}

void GuiController::requestStop() {
  if (!started_.load(std::memory_order_acquire)) {
    return;
  }
  stop_requested_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(state_mu_);
  if (ctx_) {
    ctx_->stop_flag.store(true, std::memory_order_release);
  }
}

void GuiController::wait() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

GuiStats GuiController::getStats() const {
  GuiStats out;
  SolverContext* ctx_ptr = nullptr;
  bool use_final = false;
  SolverStats final_snapshot{};
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (terminal_state_ != GuiState::Idle) {
      out.state = terminal_state_;
      out.last_error = last_error_;
      if (has_final_stats_) {
        use_final = true;
        final_snapshot = final_stats_;
      }
    } else if (started_.load(std::memory_order_acquire)) {
      out.state = GuiState::Running;
    } else {
      out.state = GuiState::Idle;
    }
    ctx_ptr = ctx_.get();
  }

  if (use_final) {
    out.chunks_scanned    = final_snapshot.chunks_scanned;
    out.keys_scanned      = final_snapshot.keys_scanned;
    out.elapsed_seconds   = final_snapshot.elapsed_seconds;
    out.rate_keys_per_sec = final_snapshot.rate_keys_per_sec;
    out.matches_found     = final_snapshot.matches_found;
    return out;
  }

  // Note: ctx_ptr->solver->getStats() is already thread-safe (atomics +
  // steady_clock::now), and ctx_ outlives worker_ until ~GuiController().
  if (ctx_ptr && ctx_ptr->solver) {
    SolverStats ss = ctx_ptr->solver->getStats();
    out.chunks_scanned    = ss.chunks_scanned;
    out.keys_scanned      = ss.keys_scanned;
    out.elapsed_seconds   = ss.elapsed_seconds;
    out.rate_keys_per_sec = ss.rate_keys_per_sec;
    out.matches_found     = ss.matches_found;
  }

  return out;
}

std::filesystem::path GuiController::foundMatchesPath() const {
  return options_.output_dir / "found_matches.jsonl";
}

std::filesystem::path GuiController::scannedRangesPath() const {
  return options_.output_dir / "scanned_ranges.jsonl";
}

}  // namespace puzzle::gui

