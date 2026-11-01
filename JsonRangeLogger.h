#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RangeKey.h"

namespace puzzle {

enum class RangeStatus {
  Completed,
  Aborted
};

struct RangeRecord {
  RangeKey start;
  RangeKey end;
  uint64_t requested_keys = 0;  // what the generator was asked for (chunk_size)
  // Actual keys scanned for this chunk, as reported by the engine.
  // For status=completed: scanned_keys == the chunk's effective length.
  // For status=aborted: scanned_keys may be LESS than the chunk length; resume
  // logic MUST read scanned_keys (not the chunk length) and continue the
  // interval at start + scanned_keys. Never assume an aborted chunk scanned
  // zero keys, and never assume it scanned the whole interval.
  uint64_t scanned_keys = 0;
  std::string timestamp;  // ISO-8601 UTC (auto-filled if empty)
  RangeStatus status = RangeStatus::Completed;
  int gpu_id = 0;
  bool endomorphism_covered = false;
};

struct LoggerConfig {
  std::filesystem::path directory;
  std::string base_name = "scanned_ranges";
  size_t flush_every_n = 10;                     // batching updates (Scenario 3)
  uint64_t rotate_bytes = 100ull * 1024 * 1024;  // rotate when file exceeds limit
};

struct LoadReport {
  size_t files_read = 0;
  size_t records_loaded = 0;
  size_t malformed_lines_skipped = 0;
};

class JsonRangeLogger {
 public:
  explicit JsonRangeLogger(const LoggerConfig& config);
  ~JsonRangeLogger();

  JsonRangeLogger(const JsonRangeLogger&) = delete;
  JsonRangeLogger& operator=(const JsonRangeLogger&) = delete;

  // Append a range record.
  // Note: duplicate identity is start-key only.
  // CAVEAT: If the architecture ever moves overlap detection into the logger,
  // this duplicate identity must become (start, end).
  // If record.timestamp is empty, the logger fills it with the current
  // UTC time in ISO-8601 format with millisecond precision
  // (e.g. 2026-10-30T15:45:00.123Z) before serialising.
  // Returns true if accepted and buffered/written, false if start key was already seen.
  bool append(const RangeRecord& record);

  // Explicitly flush buffered records to disk.
  void flush();

  // Query if a start key is in the duplicate-prevention set.
  bool contains(const RangeKey& start) const;

  // Total unique records recorded (loaded + appended).
  size_t size() const;

  // Read all records currently known.
  std::vector<RangeRecord> records() const;

  // Statistics from loading rotated & active files during construction.
  LoadReport loadReport() const;

 private:
  LoggerConfig config_;
  LoadReport load_report_;

  mutable std::mutex mutex_;
  std::unordered_set<RangeKey> seen_starts_;
  std::unordered_map<RangeKey, size_t> start_to_index_;
  std::vector<RangeRecord> all_records_;
  std::vector<RangeRecord> buffer_;

  std::ofstream out_file_;
  uint64_t current_file_size_ = 0;

  void loadExistingFilesLocked();
  void rotateIfNeededLocked(size_t next_line_size);
  void flushBufferLocked();
  std::filesystem::path activeFilePath() const;
};

}  // namespace puzzle
