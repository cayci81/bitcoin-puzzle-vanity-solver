#pragma once

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

#include "PrefixFilter.h"

namespace puzzle {

struct FoundMatchRecord {
  std::string address;
  MatchKind kind = MatchKind::None;
  std::string matched_prefix;
  std::string private_key_hex;        // only non-empty for MatchKind::Full
  std::string timestamp;              // ISO-8601 UTC
  int gpu_id = 0;
};

struct MatchWriterConfig {
  std::filesystem::path output_path = "found_matches.jsonl";
  std::atomic<bool>* stop_flag = nullptr;
  // Optional custom sync hook for testing fsync/commit execution order
  std::function<void(int fd)> sync_hook = nullptr;
  // Optional custom write hook called INSTEAD of fwrite. Used by tests
  // to simulate write failure. Production leaves nullptr.
  std::function<void(const std::string& line)> write_hook = nullptr;
};

class FoundMatchWriter {
 public:
  // Throws std::runtime_error if output_path cannot be opened or written to.
  explicit FoundMatchWriter(const MatchWriterConfig& config);
  // DESTRUCTOR CONTRACT: Not thread-safe against recordMatch. The caller
  // MUST ensure all worker threads have joined and no thread is inside
  // recordMatch() when the destructor runs. fclose() racing with an
  // in-flight fwrite()/fsync() is undefined behavior and can corrupt or
  // lose a full-match record.
  ~FoundMatchWriter();

  FoundMatchWriter(const FoundMatchWriter&) = delete;
  FoundMatchWriter& operator=(const FoundMatchWriter&) = delete;

  // Thread-safe logging of matches.
  // If match.timestamp is empty, it is auto-filled with the current UTC
  // ISO-8601 timestamp before writing.
  // Prefix matches: logs address, kind="prefix", matched_prefix (NO private key logged).
  // Full matches:
  //   1. Serialize complete record with private_key_hex.
  //   2. Prints JSON line immediately to stderr and flushes stderr.
  //   3. Writes JSON line to file (via write_hook or fwrite).
  //   4. Flushes FILE buffer to OS via fflush.
  //   5. Forces OS-level commit (sync_hook or _commit/fsync) before returning.
  //   6. Sets stop_flag to true to signal clean shutdown of all worker threads.
  // Throws std::runtime_error if writing or syncing fails. Before throwing,
  // the JSON line is already written to stderr, and stop_flag (if set) is raised.
  // The exception message contains the address only (not the private key).
  void recordMatch(const FoundMatchRecord& match);

  // Path of the current match file.
  const std::filesystem::path& outputPath() const;

  // Path of the human-readable vanity prefix match file (prefix_found.txt).
  std::filesystem::path prefixOutputPath() const;

 private:
  MatchWriterConfig config_;
  std::filesystem::path prefix_output_path_;
  mutable std::mutex mutex_;
  std::FILE* file_ = nullptr;
  std::FILE* prefix_file_ = nullptr;
};

}  // namespace puzzle
