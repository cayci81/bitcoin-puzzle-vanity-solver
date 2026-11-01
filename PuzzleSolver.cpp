#include "PuzzleSolver.h"

#include <cassert>
#include <chrono>

namespace puzzle {

PuzzleSolver::PuzzleSolver(std::shared_ptr<IEngine> engine,
                           std::shared_ptr<RandomChunkGenerator> generator,
                           std::shared_ptr<JsonRangeLogger> logger,
                           std::shared_ptr<AddressMatcher> matcher,
                           std::shared_ptr<FoundMatchWriter> writer,
                           const SolverConfig& config)
    : engine_(std::move(engine)),
      generator_(std::move(generator)),
      logger_(std::move(logger)),
      matcher_(std::move(matcher)),
      writer_(std::move(writer)),
      config_(config) {}

SolverExitReason PuzzleSolver::run() {
  finished_.store(false, std::memory_order_release);
  if (!started_.load(std::memory_order_acquire)) {
    start_time_ = std::chrono::steady_clock::now();
    started_.store(true, std::memory_order_release);
  }
  struct FinishGuard {
    std::chrono::steady_clock::time_point& end_time;
    std::atomic<bool>& finished;
    ~FinishGuard() {
      end_time = std::chrono::steady_clock::now();
      finished.store(true, std::memory_order_release);
    }
  } finish_guard{end_time_, finished_};

  // STOP_FLAG ENTRY CONTRACT: Exit immediately if stop_flag is already raised.
  if (config_.stop_flag && config_.stop_flag->load(std::memory_order_acquire)) {
    return SolverExitReason::StoppedExternal;
  }

  // IEngine::scan takes the stop flag by reference. If the config omits one,
  // poll a local dummy that is never raised so the engine contract is upheld.
  std::atomic<bool> dummy_stop{false};
  std::atomic<bool>& stop = config_.stop_flag ? *config_.stop_flag : dummy_stop;

  while (true) {
    if (stop.load(std::memory_order_acquire)) {
      return SolverExitReason::StoppedExternal;
    }

    if (config_.max_chunks > 0 && chunks_scanned_.load(std::memory_order_relaxed) >= config_.max_chunks) {
      return SolverExitReason::Exhausted;
    }

    auto opt_chunk = generator_->next();
    if (!opt_chunk) {
      return SolverExitReason::Exhausted;
    }

    Chunk chunk = *opt_chunk;
    EngineBatchResult result;
    if (engine_->reportsLiveProgress()) {
      engine_->setProgressCounter(&keys_scanned_);
      try {
        result = engine_->scan(chunk, config_.gpu_table, stop);
      } catch (...) {
        engine_->setProgressCounter(nullptr);
        throw;
      }
      engine_->setProgressCounter(nullptr);
      // Per-key progress already incremented keys_scanned_ for this chunk.
      // Do NOT double-add. Sanity-check in debug builds:
      assert(keys_scanned_.load(std::memory_order_relaxed) >= result.keys_scanned);
    } else {
      result = engine_->scan(chunk, config_.gpu_table, stop);
      keys_scanned_.fetch_add(result.keys_scanned, std::memory_order_relaxed);
    }

    bool full_match_interrupted = false;

    for (size_t i = 0; i < result.matched_addresses.size(); ++i) {
      const std::string& addr = result.matched_addresses[i];
      MatchResult match_res = matcher_->classify(addr);

      if (match_res.kind == MatchKind::Prefix) {
        matches_found_.fetch_add(1, std::memory_order_relaxed);
        FoundMatchRecord rec;
        rec.address = addr;
        rec.kind = MatchKind::Prefix;
        rec.matched_prefix = match_res.matched_prefix;
        rec.gpu_id = config_.worker_id;
        // Never pass private key for prefix match
        writer_->recordMatch(rec);
      } else if (match_res.kind == MatchKind::Full) {
        matches_found_.fetch_add(1, std::memory_order_relaxed);
        FoundMatchRecord rec;
        rec.address = addr;
        rec.kind = MatchKind::Full;
        rec.matched_prefix = match_res.matched_prefix;
        rec.gpu_id = config_.worker_id;
        if (i < result.matched_privkeys_hex.size()) {
          rec.private_key_hex = result.matched_privkeys_hex[i];
        }
        writer_->recordMatch(rec);  // recordMatch sets stop_flag
        full_match_interrupted = true;
        break;
      }
    }

    RangeRecord log_rec;
    log_rec.start = chunk.start;
    log_rec.end = chunk.end;
    log_rec.requested_keys = chunk.requested_keys;
    // Self-describing log: record what the engine ACTUALLY scanned, not the
    // chunk's nominal size. For completed chunks the two are equal; for
    // aborted chunks scanned_keys < chunk length, and resume logic can
    // continue at start + scanned_keys (see RangeRecord doc in JsonRangeLogger.h).
    log_rec.scanned_keys = result.keys_scanned;
    log_rec.gpu_id = config_.worker_id;

    // STATUS DECISION:
    //  - Full match interrupted this chunk        -> aborted, FullMatchFound.
    //  - Engine returned early (aborted_early)    -> aborted, StoppedExternal.
    //    because stop_flag was raised externally.
    //  - Otherwise the chunk scanned to completion (with or without prefix
    //    matches)                               -> completed, keep looping.
    if (full_match_interrupted) {
      log_rec.status = RangeStatus::Aborted;
      logger_->append(log_rec);
      chunks_scanned_.fetch_add(1, std::memory_order_relaxed);
      return SolverExitReason::FullMatchFound;
    }

    if (result.aborted_early) {
      log_rec.status = RangeStatus::Aborted;
      logger_->append(log_rec);
      chunks_scanned_.fetch_add(1, std::memory_order_relaxed);
      return SolverExitReason::StoppedExternal;
    }

    log_rec.status = RangeStatus::Completed;
    logger_->append(log_rec);
    chunks_scanned_.fetch_add(1, std::memory_order_relaxed);
  }
}

SolverStats PuzzleSolver::getStats() const {
  SolverStats stats;
  stats.chunks_scanned = chunks_scanned_.load(std::memory_order_relaxed);
  stats.keys_scanned = keys_scanned_.load(std::memory_order_relaxed);
  stats.matches_found = matches_found_.load(std::memory_order_relaxed);

  if (started_.load(std::memory_order_acquire)) {
    const auto end = finished_.load(std::memory_order_acquire)
                         ? end_time_
                         : std::chrono::steady_clock::now();
    stats.elapsed_seconds = std::chrono::duration<double>(end - start_time_).count();
    if (stats.elapsed_seconds > 0.0) {
      stats.rate_keys_per_sec = static_cast<double>(stats.keys_scanned) / stats.elapsed_seconds;
    }
  }

  return stats;
}

}  // namespace puzzle
