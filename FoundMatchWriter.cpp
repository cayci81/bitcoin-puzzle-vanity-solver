#include "FoundMatchWriter.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#if defined(_WIN32)
#include <io.h>
#define FILENO_FN _fileno
#define COMMIT_FN _commit
#else
#include <unistd.h>
#define FILENO_FN fileno
#define COMMIT_FN fsync
#endif

namespace puzzle {

using nlohmann::json;

namespace {

std::string FormatUtcTimestamp(const std::chrono::system_clock::time_point& tp) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
  std::time_t tt = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>(ms.count()));
  return buf;
}

}  // namespace

FoundMatchWriter::FoundMatchWriter(const MatchWriterConfig& config)
    : config_(config),
      prefix_output_path_(
          config_.output_path.has_parent_path()
              ? (config_.output_path.parent_path() / "prefix_found.txt")
              : std::filesystem::path("prefix_found.txt")) {
  std::error_code ec;
  if (config_.output_path.empty() || std::filesystem::is_directory(config_.output_path, ec)) {
    throw std::runtime_error("FoundMatchWriter: invalid or directory output path: " +
                             config_.output_path.string());
  }
  if (config_.output_path.has_parent_path()) {
    std::filesystem::create_directories(config_.output_path.parent_path(), ec);
  }
#ifdef _WIN32
  file_ = _wfopen(config_.output_path.c_str(), L"ab");
#else
  file_ = std::fopen(config_.output_path.string().c_str(), "ab");
#endif
  if (!file_) {
    throw std::runtime_error("FoundMatchWriter: cannot open output path: " +
                             config_.output_path.string());
  }
}

FoundMatchWriter::~FoundMatchWriter() {
  if (prefix_file_) {
    std::fclose(prefix_file_);
    prefix_file_ = nullptr;
  }
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

const std::filesystem::path& FoundMatchWriter::outputPath() const {
  return config_.output_path;
}

std::filesystem::path FoundMatchWriter::prefixOutputPath() const {
  return prefix_output_path_;
}

void FoundMatchWriter::recordMatch(const FoundMatchRecord& match) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string ts = match.timestamp;
  if (ts.empty()) {
    ts = FormatUtcTimestamp(std::chrono::system_clock::now());
  }

  json j;
  j["address"] = match.address;
  j["kind"] = (match.kind == MatchKind::Full) ? "full" : "prefix";
  j["matched_prefix"] = match.matched_prefix;
  if (match.kind == MatchKind::Full) {
    j["private_key_hex"] = match.private_key_hex;
  }
  j["timestamp"] = ts;
  j["gpu_id"] = match.gpu_id;

  std::string line = j.dump();

  if (match.kind == MatchKind::Full) {
    // Write to stderr first: if file write or sync fails, operator still sees key.
    std::cerr << line << "\n";
    std::cerr.flush();

    try {
      if (config_.write_hook) {
        config_.write_hook(line);
      } else {
        std::string line_with_nl = line + "\n";
        size_t written = std::fwrite(line_with_nl.data(), 1, line_with_nl.size(), file_);
        if (written != line_with_nl.size()) {
          throw std::runtime_error("FoundMatchWriter: short write to file");
        }
      }

      if (std::fflush(file_) != 0) {
        throw std::runtime_error("FoundMatchWriter: fflush failed");
      }

      int fd = FILENO_FN(file_);
      if (config_.sync_hook) {
        config_.sync_hook(fd);
      } else {
        if (COMMIT_FN(fd) != 0) {
          throw std::runtime_error("FoundMatchWriter: sync failed");
        }
      }

      if (config_.stop_flag) {
        config_.stop_flag->store(true, std::memory_order_release);
      }
    } catch (...) {
      if (config_.stop_flag) {
        config_.stop_flag->store(true, std::memory_order_release);
      }
      throw std::runtime_error("FoundMatchWriter: failed to record full match for address: " +
                               match.address);
    }
  } else {
    // Prefix match: NO private key in found_matches.jsonl, no stderr, no sync, no stop_flag.
    if (config_.write_hook) {
      config_.write_hook(line);
    } else {
      std::string line_with_nl = line + "\n";
      size_t written = std::fwrite(line_with_nl.data(), 1, line_with_nl.size(), file_);
      if (written != line_with_nl.size()) {
        throw std::runtime_error("FoundMatchWriter: short write for prefix match");
      }
    }
    std::fflush(file_);

    // Also append human-readable vanity entry to prefix_found.txt
    if (!prefix_file_) {
#ifdef _WIN32
      prefix_file_ = _wfopen(prefix_output_path_.c_str(), L"ab");
#else
      prefix_file_ = std::fopen(prefix_output_path_.string().c_str(), "ab");
#endif
      if (!prefix_file_) {
        throw std::runtime_error("FoundMatchWriter: cannot open prefix_found.txt: " +
                                 prefix_output_path_.string());
      }
    }
    std::string txt_line = "[" + ts + "] Address: " + match.address +
                           " | Matched Prefix: " + match.matched_prefix +
                           " | Private Key: " + match.private_key_hex + "\n";
    size_t txt_written =
        std::fwrite(txt_line.data(), 1, txt_line.size(), prefix_file_);
    if (txt_written != txt_line.size()) {
      throw std::runtime_error("FoundMatchWriter: short write to prefix_found.txt");
    }
    std::fflush(prefix_file_);
  }
}

}  // namespace puzzle
