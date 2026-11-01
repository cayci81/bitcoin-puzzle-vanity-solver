#include "JsonRangeLogger.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>

namespace puzzle {

namespace fs = std::filesystem;
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
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           static_cast<int>(ms.count()));
  return buf;
}

std::string FormatRotationTimestamp(const std::chrono::system_clock::time_point& tp) {
  // Required format: scanned_ranges.YYYYMMDDTHHMMSSmmmZ.jsonl (8 digits date + 'T' + 9 digits time + 'Z')
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
  std::time_t tt = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[64];
  snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02d%03dZ",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           static_cast<int>(ms.count()));
  return buf;
}

std::string SerializeRecord(const RangeRecord& r) {
  json j;
  j["start_hex"] = r.start.ToHex();
  j["end_hex"] = r.end.ToHex();
  j["requested_keys"] = r.requested_keys;
  j["scanned_keys"] = r.scanned_keys;
  j["timestamp"] = r.timestamp;
  j["status"] = (r.status == RangeStatus::Aborted) ? "aborted" : "completed";
  j["gpu_id"] = r.gpu_id;
  j["endomorphism_covered"] = r.endomorphism_covered;
  return j.dump() + "\n";
}

bool IsWhitespaceOnly(const std::string& s) {
  for (char c : s) {
    if (!std::isspace(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

}  // namespace

JsonRangeLogger::JsonRangeLogger(const LoggerConfig& config) : config_(config) {
  fs::create_directories(config_.directory);
  std::lock_guard<std::mutex> lock(mutex_);
  loadExistingFilesLocked();
}

JsonRangeLogger::~JsonRangeLogger() {
  std::lock_guard<std::mutex> lock(mutex_);
  try {
    flushBufferLocked();
  } catch (...) {
    // Destructor must not throw.
  }
  if (out_file_.is_open()) {
    out_file_.close();
  }
}

fs::path JsonRangeLogger::activeFilePath() const {
  return config_.directory / (config_.base_name + ".jsonl");
}

void JsonRangeLogger::loadExistingFilesLocked() {
  std::vector<fs::path> matching_files;
  // Match ONLY active file <base_name>.jsonl or rotated <base_name>.<timestamp>[-<n>].jsonl
  const std::regex valid_file_pattern(
      "^" + config_.base_name + "(\\.\\d{8}T\\d{9}Z(-\\d+)?)?\\.jsonl$");

  for (const auto& entry : fs::directory_iterator(config_.directory)) {
    if (!entry.is_regular_file()) continue;
    std::string name = entry.path().filename().string();
    if (std::regex_match(name, valid_file_pattern)) {
      matching_files.push_back(entry.path());
    }
  }

  // Sort so rotated files load before active file
  std::sort(matching_files.begin(), matching_files.end());

  fs::path active = activeFilePath();

  for (const auto& file_path : matching_files) {
    load_report_.files_read++;
    std::ifstream in(file_path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || IsWhitespaceOnly(line)) {
        continue;
      }

      try {
        json j = json::parse(line);
        if (!j.contains("start_hex") || !j.contains("end_hex") ||
            !j.contains("requested_keys") || !j.contains("scanned_keys") ||
            !j.contains("timestamp") || !j.contains("status") ||
            !j.contains("gpu_id") || !j.contains("endomorphism_covered")) {
          load_report_.malformed_lines_skipped++;
          continue;
        }

        RangeRecord r;
        r.start = RangeKey::FromHex(j.at("start_hex").get<std::string>());
        r.end = RangeKey::FromHex(j.at("end_hex").get<std::string>());
        r.requested_keys = j.at("requested_keys").get<uint64_t>();
        r.scanned_keys = j.at("scanned_keys").get<uint64_t>();
        r.timestamp = j.at("timestamp").get<std::string>();
        std::string status_str = j.at("status").get<std::string>();
        r.status = (status_str == "aborted") ? RangeStatus::Aborted : RangeStatus::Completed;
        r.gpu_id = j.at("gpu_id").get<int>();
        r.endomorphism_covered = j.at("endomorphism_covered").get<bool>();

        auto it = start_to_index_.find(r.start);
        if (it == start_to_index_.end()) {
          start_to_index_[r.start] = all_records_.size();
          seen_starts_.insert(r.start);
          all_records_.push_back(r);
          load_report_.records_loaded++;
        } else if (all_records_[it->second].status == RangeStatus::Aborted &&
                   r.status == RangeStatus::Completed) {
          all_records_[it->second] = r;
        }
      } catch (...) {
        load_report_.malformed_lines_skipped++;
      }
    }
  }

  // Check active file size and ensure it ends with newline if non-empty
  if (fs::exists(active)) {
    uint64_t sz = fs::file_size(active);
    if (sz > 0) {
      std::ifstream in(active, std::ios::binary);
      in.seekg(-1, std::ios::end);
      char last_char = 0;
      in.get(last_char);
      if (last_char != '\n') {
        in.close();
        // This write is intentional: repairs a crash-truncated tail so the next
        // append starts on a fresh line; harmless if raced (adds a blank line,
        // which the loader ignores).
        std::ofstream fix_out(active, std::ios::binary | std::ios::app);
        fix_out.put('\n');
        fix_out.close();
        sz++;
      }
    }
    current_file_size_ = sz;
  } else {
    current_file_size_ = 0;
  }
}

bool JsonRangeLogger::append(const RangeRecord& record) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = start_to_index_.find(record.start);
  if (it != start_to_index_.end()) {
    if (!(all_records_[it->second].status == RangeStatus::Aborted &&
          record.status == RangeStatus::Completed)) {
      return false;
    }
  }

  RangeRecord r = record;
  if (r.timestamp.empty()) {
    r.timestamp = FormatUtcTimestamp(std::chrono::system_clock::now());
  }

  if (it != start_to_index_.end()) {
    all_records_[it->second] = r;
  } else {
    start_to_index_[r.start] = all_records_.size();
    seen_starts_.insert(r.start);
    all_records_.push_back(r);
  }
  buffer_.push_back(r);

  if (buffer_.size() >= config_.flush_every_n) {
    flushBufferLocked();
  }

  return true;
}

void JsonRangeLogger::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  flushBufferLocked();
}

bool JsonRangeLogger::contains(const RangeKey& start) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return seen_starts_.count(start) > 0;
}

size_t JsonRangeLogger::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return all_records_.size();
}

std::vector<RangeRecord> JsonRangeLogger::records() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return all_records_;
}

LoadReport JsonRangeLogger::loadReport() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return load_report_;
}

void JsonRangeLogger::rotateIfNeededLocked(size_t next_line_size) {
  if (current_file_size_ > 0 && (current_file_size_ + next_line_size > config_.rotate_bytes)) {
    if (out_file_.is_open()) {
      out_file_.close();
    }

    auto now = std::chrono::system_clock::now();
    std::string base_ts = FormatRotationTimestamp(now);
    fs::path rotated_path = config_.directory / (config_.base_name + "." + base_ts + ".jsonl");

    // Collision loop if rotation occurs rapidly within same millisecond
    int suffix = 1;
    while (fs::exists(rotated_path)) {
      rotated_path = config_.directory / (config_.base_name + "." + base_ts + "-" + std::to_string(suffix++) + ".jsonl");
    }

    fs::path active = activeFilePath();
    if (fs::exists(active)) {
      fs::rename(active, rotated_path);
    }
    current_file_size_ = 0;
  }
}

void JsonRangeLogger::flushBufferLocked() {
  if (buffer_.empty()) return;

  size_t written_count = 0;
  try {
    for (const auto& r : buffer_) {
      std::string line = SerializeRecord(r);
      rotateIfNeededLocked(line.size());

      if (!out_file_.is_open()) {
        out_file_.clear();
        out_file_.open(activeFilePath(), std::ios::binary | std::ios::app);
      }
      if (!out_file_.is_open() || !out_file_.good()) {
        throw std::runtime_error(
            "JsonRangeLogger: failed to open log file for writing: " +
            activeFilePath().string());
      }

      out_file_.write(line.data(), static_cast<std::streamsize>(line.size()));
      if (!out_file_.good()) {
        throw std::runtime_error(
            "JsonRangeLogger: failed to write record to log file: " +
            activeFilePath().string());
      }
      current_file_size_ += line.size();
      ++written_count;
    }

    if (out_file_.is_open()) {
      out_file_.flush();
      if (!out_file_.good()) {
        throw std::runtime_error(
            "JsonRangeLogger: failed to flush log file: " +
            activeFilePath().string());
      }
    }
    buffer_.clear();
  } catch (...) {
    if (out_file_.is_open()) {
      out_file_.close();
    }
    out_file_.clear();
    if (written_count > 0 && written_count < buffer_.size()) {
      buffer_.erase(
          buffer_.begin(),
          buffer_.begin() + static_cast<std::ptrdiff_t>(written_count));
    }
    throw;
  }
}

}  // namespace puzzle
