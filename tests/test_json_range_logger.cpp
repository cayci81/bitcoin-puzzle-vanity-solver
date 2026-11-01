// Unit tests for puzzle::JsonRangeLogger (TDD - no implementation exists yet).
//
// API contract exercised here (header "JsonRangeLogger.h"):
//
//   enum class RangeStatus { Completed, Aborted };   // JSON: "completed" / "aborted"
//   struct RangeRecord {
//     RangeKey start, end;                // inclusive
//     uint64_t requested_keys, scanned_keys;
//     std::string timestamp;              // ISO-8601 UTC; logger fills it when empty
//     RangeStatus status = RangeStatus::Completed;
//     int  gpu_id = 0;
//     bool endomorphism_covered = false;
//   };
//   struct LoggerConfig {
//     fs::path directory;                 // created if missing
//     std::string base_name = "scanned_ranges";
//     size_t   flush_every_n = 10;        // batch size (I/O scenario 3)
//     uint64_t rotate_bytes  = 100ull * 1024 * 1024;
//   };
//   struct LoadReport { size_t files_read, records_loaded, malformed_lines_skipped; };
//   class JsonRangeLogger {
//     explicit JsonRangeLogger(const LoggerConfig&);   // loads ALL files, builds duplicate set
//     ~JsonRangeLogger();                              // flushes pending records
//     bool   append(const RangeRecord&);               // false => duplicate start key, not written
//     void   flush();
//     bool   contains(const RangeKey& start) const;
//     size_t size() const;
//     std::vector<RangeRecord> records() const;        // load order, then append order
//     LoadReport loadReport() const;
//   };
//
// File layout:
//   <dir>/scanned_ranges.jsonl                      active file, one JSON object per line
//   <dir>/scanned_ranges.<UTC yyyymmddThhmmssmmmZ>[-<n>].jsonl   rotated (n = collision suffix)
//   Files that do not match this pattern in <dir> are ignored.
//
// Record JSON keys: start_hex, end_hex, requested_keys, scanned_keys, timestamp,
//                   status, gpu_id, endomorphism_covered.
// Hex: lowercase, no "0x" prefix.
// Duplicate identity: the start key (a given start may be scanned once).
// Rotation rule: before writing a line, if current_size + line_size > rotate_bytes and the
//                file is non-empty, flush + close + rename the file to the rotated name and
//                start a fresh scanned_ranges.jsonl.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <thread>

#include "JsonRangeLogger.h"
#include "test_support.h"

using namespace puzzle;
using namespace puzzle::test;
using nlohmann::json;

namespace {

LoggerConfig Cfg(const fs::path& dir, size_t flush_every = 1, uint64_t rotate = 100ull << 20) {
  LoggerConfig c;
  c.directory = dir;
  c.flush_every_n = flush_every;
  c.rotate_bytes = rotate;
  return c;
}

// i-th distinct record: consecutive 1000-key chunks from the puzzle start.
RangeRecord MakeRecord(uint64_t i, uint64_t scanned = 1000) {
  RangeRecord r;
  r.start = RK("400000000000000000").plus(i * scanned);
  r.end = r.start.plus(scanned - 1);
  r.requested_keys = scanned + 17;
  r.scanned_keys = scanned;
  r.timestamp = "2026-10-30T15:45:00.123Z";
  r.status = RangeStatus::Completed;
  r.gpu_id = static_cast<int>(i % 4);
  r.endomorphism_covered = (i % 2 == 1);
  return r;
}

std::vector<fs::path> LoggerFiles(const fs::path& dir) {
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir)) {
    const std::string n = e.path().filename().string();
    if (n.rfind("scanned_ranges", 0) == 0 && e.path().extension() == ".jsonl")
      files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

size_t TotalLines(const fs::path& dir) {
  size_t n = 0;
  for (const auto& f : LoggerFiles(dir)) n += ReadLines(f).size();
  return n;
}

}  // namespace

// ---------------------------------------------------------------------- schema

TEST(JsonRangeLoggerSchema, EachRecordIsOneSelfContainedJsonLine) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path()));
    ASSERT_TRUE(log.append(MakeRecord(0)));
    ASSERT_TRUE(log.append(MakeRecord(1)));
  }
  auto lines = ReadLines(tmp.path() / "scanned_ranges.jsonl");
  ASSERT_EQ(lines.size(), 2u);
  for (const auto& l : lines) EXPECT_NO_THROW((void)json::parse(l)) << l;
}

TEST(JsonRangeLoggerSchema, FieldNamesAndTypes) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path()));
    log.append(MakeRecord(1));  // i=1 -> gpu 1, endomorphism_covered=true
  }
  json j = json::parse(ReadLines(tmp.path() / "scanned_ranges.jsonl").at(0));
  EXPECT_TRUE(RK(j.at("start_hex").get<std::string>()) == RK("400000000000000000").plus(1000));
  EXPECT_TRUE(RK(j.at("end_hex").get<std::string>()) == RK("400000000000000000").plus(1999));
  EXPECT_TRUE(std::regex_match(j.at("start_hex").get<std::string>(), std::regex("^[0-9a-f]+$")));
  EXPECT_TRUE(std::regex_match(j.at("end_hex").get<std::string>(), std::regex("^[0-9a-f]+$")));
  EXPECT_TRUE(j.at("requested_keys").is_number_unsigned());
  EXPECT_EQ(j.at("requested_keys").get<uint64_t>(), 1017u);
  EXPECT_TRUE(j.at("scanned_keys").is_number_unsigned());
  EXPECT_EQ(j.at("scanned_keys").get<uint64_t>(), 1000u);
  EXPECT_EQ(j.at("timestamp").get<std::string>(), "2026-10-30T15:45:00.123Z");
  EXPECT_EQ(j.at("status").get<std::string>(), "completed");
  EXPECT_EQ(j.at("gpu_id").get<int>(), 1);
  EXPECT_TRUE(j.at("endomorphism_covered").is_boolean());
  EXPECT_TRUE(j.at("endomorphism_covered").get<bool>());
}

TEST(JsonRangeLoggerSchema, AbortedStatusSerialisesAsString) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path()));
    auto r = MakeRecord(0);
    r.status = RangeStatus::Aborted;
    log.append(r);
  }
  json j = json::parse(ReadLines(tmp.path() / "scanned_ranges.jsonl").at(0));
  EXPECT_EQ(j.at("status").get<std::string>(), "aborted");
}

TEST(JsonRangeLoggerSchema, EmptyTimestampIsFilledWithIso8601Utc) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path()));
    auto r = MakeRecord(0);
    r.timestamp.clear();
    log.append(r);
  }
  json j = json::parse(ReadLines(tmp.path() / "scanned_ranges.jsonl").at(0));
  EXPECT_TRUE(std::regex_match(
      j.at("timestamp").get<std::string>(),
      std::regex(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z$)")));
}

// ------------------------------------------------------------------- round trip

TEST(JsonRangeLoggerRoundTrip, ReopenRestoresAllFieldsInOrder) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path()));
    for (uint64_t i = 0; i < 5; ++i) ASSERT_TRUE(log.append(MakeRecord(i)));
  }
  JsonRangeLogger reopened(Cfg(tmp.path()));
  auto recs = reopened.records();
  ASSERT_EQ(recs.size(), 5u);
  for (uint64_t i = 0; i < 5; ++i) {
    auto expected = MakeRecord(i);
    EXPECT_TRUE(recs[i].start == expected.start) << i;
    EXPECT_TRUE(recs[i].end == expected.end) << i;
    EXPECT_EQ(recs[i].requested_keys, expected.requested_keys);
    EXPECT_EQ(recs[i].scanned_keys, expected.scanned_keys);
    EXPECT_EQ(recs[i].timestamp, expected.timestamp);
    EXPECT_EQ(recs[i].status, expected.status);
    EXPECT_EQ(recs[i].gpu_id, expected.gpu_id);
    EXPECT_EQ(recs[i].endomorphism_covered, expected.endomorphism_covered);
  }
  EXPECT_EQ(reopened.loadReport().records_loaded, 5u);
  EXPECT_EQ(reopened.loadReport().malformed_lines_skipped, 0u);
}

TEST(JsonRangeLoggerRoundTrip, MissingDirectoryIsCreatedAndStartsEmpty) {
  TempDir tmp;
  const fs::path nested = tmp.path() / "a" / "b";
  JsonRangeLogger log(Cfg(nested));
  EXPECT_TRUE(fs::is_directory(nested));
  EXPECT_EQ(log.size(), 0u);
}

// ------------------------------------------------------------------ duplicates

TEST(JsonRangeLoggerDuplicates, SecondAppendOfSameStartIsRejectedAndNotWritten) {
  TempDir tmp;
  JsonRangeLogger log(Cfg(tmp.path()));
  EXPECT_TRUE(log.append(MakeRecord(3)));
  EXPECT_FALSE(log.append(MakeRecord(3)));
  EXPECT_EQ(log.size(), 1u);
  EXPECT_TRUE(log.contains(MakeRecord(3).start));
  EXPECT_FALSE(log.contains(MakeRecord(4).start));
  EXPECT_EQ(ReadLines(tmp.path() / "scanned_ranges.jsonl").size(), 1u);
}

TEST(JsonRangeLoggerDuplicates, ResumeBuildsDuplicateSetFromDisk) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path()));
    for (uint64_t i = 0; i < 3; ++i) log.append(MakeRecord(i));
  }
  JsonRangeLogger resumed(Cfg(tmp.path()));
  EXPECT_TRUE(resumed.contains(MakeRecord(2).start));
  EXPECT_FALSE(resumed.append(MakeRecord(2)));
  EXPECT_TRUE(resumed.append(MakeRecord(3)));
  EXPECT_EQ(resumed.size(), 4u);
}

TEST(JsonRangeLoggerDuplicates, CompletedRecordSupersedesAbortedRecordForSameStart) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path()));
    RangeRecord aborted = MakeRecord(5);
    aborted.status = RangeStatus::Aborted;
    aborted.scanned_keys = 400;
    EXPECT_TRUE(log.append(aborted));
    EXPECT_TRUE(log.contains(aborted.start));

    // Another aborted record for the same start is rejected.
    EXPECT_FALSE(log.append(aborted));

    // A completed record for the same start supersedes the aborted one.
    RangeRecord completed = MakeRecord(5);
    completed.status = RangeStatus::Completed;
    completed.scanned_keys = 1000;
    EXPECT_TRUE(log.append(completed));
    EXPECT_TRUE(log.contains(completed.start));
    EXPECT_EQ(log.size(), 1u);
    ASSERT_EQ(log.records().size(), 1u);
    EXPECT_EQ(log.records()[0].status, RangeStatus::Completed);
    EXPECT_EQ(log.records()[0].scanned_keys, 1000u);

    // Subsequent completed record for the same start is rejected.
    EXPECT_FALSE(log.append(completed));
  }

  // Reopening from disk preserves the superseded completed status.
  JsonRangeLogger reopened(Cfg(tmp.path()));
  EXPECT_EQ(reopened.size(), 1u);
  EXPECT_TRUE(reopened.contains(MakeRecord(5).start));
  ASSERT_EQ(reopened.records().size(), 1u);
  EXPECT_EQ(reopened.records()[0].status, RangeStatus::Completed);
  EXPECT_EQ(reopened.records()[0].scanned_keys, 1000u);
}

TEST(JsonRangeLoggerErrorHandling, WriteFailureThrowsRuntimeErrorAndRetainsBuffer) {
  TempDir tmp;
  // Buffer records (flush_every_n = 10) then make activePath() a directory
  // before calling flush() so opening scanned_ranges.jsonl for writing fails.
  JsonRangeLogger log(Cfg(tmp.path(), /*flush_every_n=*/10));
  ASSERT_TRUE(log.append(MakeRecord(0)));
  ASSERT_TRUE(log.append(MakeRecord(1)));

  fs::create_directory(tmp.path() / "scanned_ranges.jsonl");
  EXPECT_THROW(log.flush(), std::runtime_error);

  // Remove the blocking directory and verify that the buffered records were
  // retained and can now be flushed cleanly to disk.
  fs::remove_all(tmp.path() / "scanned_ranges.jsonl");
  EXPECT_NO_THROW(log.flush());
  EXPECT_EQ(ReadLines(tmp.path() / "scanned_ranges.jsonl").size(), 2u);
}

// ---------------------------------------------------------- batching (I/O #3)

TEST(JsonRangeLoggerBatching, WritesAreHeldBackUntilBatchIsFull) {
  TempDir tmp;
  JsonRangeLogger log(Cfg(tmp.path(), /*flush_every_n*/ 3));
  const fs::path active = tmp.path() / "scanned_ranges.jsonl";

  log.append(MakeRecord(0));
  log.append(MakeRecord(1));
  EXPECT_EQ(ReadLines(active).size(), 0u) << "2 of 3 records must still be buffered";

  log.append(MakeRecord(2));
  EXPECT_EQ(ReadLines(active).size(), 3u) << "3rd record triggers the batch flush";

  log.append(MakeRecord(3));
  EXPECT_EQ(ReadLines(active).size(), 3u);
}

TEST(JsonRangeLoggerBatching, FlushEveryOneIsImmediatelyVisible) {
  TempDir tmp;
  JsonRangeLogger log(Cfg(tmp.path(), 1));
  log.append(MakeRecord(0));
  EXPECT_EQ(ReadLines(tmp.path() / "scanned_ranges.jsonl").size(), 1u);
}

TEST(JsonRangeLoggerBatching, ExplicitFlushWritesPartialBatch) {
  TempDir tmp;
  JsonRangeLogger log(Cfg(tmp.path(), 100));
  log.append(MakeRecord(0));
  log.append(MakeRecord(1));
  log.flush();
  EXPECT_EQ(ReadLines(tmp.path() / "scanned_ranges.jsonl").size(), 2u);
}

TEST(JsonRangeLoggerBatching, DestructorFlushesPendingRecords) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path(), 100));
    for (uint64_t i = 0; i < 5; ++i) log.append(MakeRecord(i));
  }
  EXPECT_EQ(ReadLines(tmp.path() / "scanned_ranges.jsonl").size(), 5u);
}

// -------------------------------------------------------------------- rotation

TEST(JsonRangeLoggerRotation, ExceedingLimitStartsNewFileAndKeepsOldOnesIntact) {
  TempDir tmp;
  const uint64_t limit = 1024;
  {
    JsonRangeLogger log(Cfg(tmp.path(), 1, limit));
    for (uint64_t i = 0; i < 50; ++i) ASSERT_TRUE(log.append(MakeRecord(i)));
  }
  auto files = LoggerFiles(tmp.path());
  ASSERT_GE(files.size(), 2u);
  EXPECT_TRUE(fs::exists(tmp.path() / "scanned_ranges.jsonl"));
  for (const auto& f : files) {
    EXPECT_LE(fs::file_size(f), limit) << f;
    if (f.filename() != "scanned_ranges.jsonl") {
      EXPECT_TRUE(std::regex_match(
          f.filename().string(), std::regex(R"(^scanned_ranges\.\d{8}T\d{9}Z(-\d+)?\.jsonl$)")))
          << f.filename();
    }
  }
  EXPECT_EQ(TotalLines(tmp.path()), 50u) << "no record lost or duplicated by rotation";
}

TEST(JsonRangeLoggerRotation, NoRecordIsSplitAcrossFiles) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path(), 1, 700));
    for (uint64_t i = 0; i < 30; ++i) log.append(MakeRecord(i));
  }
  for (const auto& f : LoggerFiles(tmp.path()))
    for (const auto& l : ReadLines(f)) EXPECT_NO_THROW((void)json::parse(l)) << f << ": " << l;
}

TEST(JsonRangeLoggerRotation, RapidRotationsInSameMillisecondNeverOverwrite) {
  TempDir tmp;
  {
    // limit smaller than ~2 records => rotation on (almost) every append, many per ms.
    JsonRangeLogger log(Cfg(tmp.path(), 1, 300));
    for (uint64_t i = 0; i < 40; ++i) ASSERT_TRUE(log.append(MakeRecord(i)));
  }
  std::set<std::string> names;
  for (const auto& f : LoggerFiles(tmp.path())) names.insert(f.filename().string());
  EXPECT_EQ(names.size(), LoggerFiles(tmp.path()).size());
  EXPECT_EQ(TotalLines(tmp.path()), 40u);
}

TEST(JsonRangeLoggerRotation, ResumeReadsActiveAndAllRotatedFiles) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path(), 1, 1024));
    for (uint64_t i = 0; i < 50; ++i) log.append(MakeRecord(i));
  }
  ASSERT_GE(LoggerFiles(tmp.path()).size(), 2u);

  JsonRangeLogger resumed(Cfg(tmp.path(), 1, 1024));
  EXPECT_EQ(resumed.size(), 50u);
  EXPECT_EQ(resumed.loadReport().files_read, LoggerFiles(tmp.path()).size());
  for (uint64_t i = 0; i < 50; ++i) {
    EXPECT_TRUE(resumed.contains(MakeRecord(i).start)) << i;
    EXPECT_FALSE(resumed.append(MakeRecord(i))) << i;
  }
}

TEST(JsonRangeLoggerRotation, UnrelatedFilesInDirectoryAreIgnored) {
  TempDir tmp;
  WriteRaw(tmp.path() / "scanned_ranges_notes.txt", "not json\n");
  WriteRaw(tmp.path() / "found_matches.jsonl", "{\"x\":1}\n");
  WriteRaw(tmp.path() / "other.jsonl", "{\"x\":1}\n");
  JsonRangeLogger log(Cfg(tmp.path()));
  EXPECT_EQ(log.size(), 0u);
  EXPECT_EQ(log.loadReport().files_read, 0u);
}

// ------------------------------------------------------- crash/corruption safety

TEST(JsonRangeLoggerRecovery, TruncatedTrailingLineIsSkippedAndValidPrefixKept) {
  TempDir tmp;
  const std::string good0 = [&] {
    json j = {{"start_hex", MakeRecord(0).start.ToHex()}, {"end_hex", MakeRecord(0).end.ToHex()},
              {"requested_keys", 1017}, {"scanned_keys", 1000},
              {"timestamp", "2026-10-30T15:45:00.123Z"}, {"status", "completed"},
              {"gpu_id", 0}, {"endomorphism_covered", false}};
    return j.dump();
  }();
  const std::string good1 = [&] {
    json j = json::parse(good0);
    j["start_hex"] = MakeRecord(1).start.ToHex();
    j["end_hex"] = MakeRecord(1).end.ToHex();
    return j.dump();
  }();
  WriteRaw(tmp.path() / "scanned_ranges.jsonl",
           good0 + "\n" + good1 + "\n" + R"({"start_hex":"4000000000000007d0","end_)");  // no newline

  JsonRangeLogger log(Cfg(tmp.path()));
  EXPECT_EQ(log.size(), 2u);
  EXPECT_EQ(log.loadReport().malformed_lines_skipped, 1u);

  // New appends must not be glued onto the broken tail.
  ASSERT_TRUE(log.append(MakeRecord(5)));
  JsonRangeLogger again(Cfg(tmp.path()));
  EXPECT_EQ(again.size(), 3u);
  EXPECT_TRUE(again.contains(MakeRecord(5).start));
}

TEST(JsonRangeLoggerRecovery, GarbageLineInTheMiddleIsSkippedLaterLinesStillLoad) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path()));
    log.append(MakeRecord(0));
  }
  {  // append garbage + a valid record by hand
    std::ofstream out(tmp.path() / "scanned_ranges.jsonl", std::ios::app);
    out << "this is not json\n";
    json j = {{"start_hex", MakeRecord(1).start.ToHex()}, {"end_hex", MakeRecord(1).end.ToHex()},
              {"requested_keys", 1000}, {"scanned_keys", 1000},
              {"timestamp", "2026-10-30T15:45:00Z"}, {"status", "completed"},
              {"gpu_id", 0}, {"endomorphism_covered", false}};
    out << j.dump() << "\n";
  }
  JsonRangeLogger log(Cfg(tmp.path()));
  EXPECT_EQ(log.size(), 2u);
  EXPECT_EQ(log.loadReport().malformed_lines_skipped, 1u);
}

TEST(JsonRangeLoggerRecovery, LineMissingRequiredFieldCountsAsMalformed) {
  TempDir tmp;
  WriteRaw(tmp.path() / "scanned_ranges.jsonl", "{\"start_hex\":\"400000000000000000\"}\n");
  JsonRangeLogger log(Cfg(tmp.path()));
  EXPECT_EQ(log.size(), 0u);
  EXPECT_EQ(log.loadReport().malformed_lines_skipped, 1u);
}

TEST(JsonRangeLoggerRecovery, BlankLinesAreIgnoredNotCountedAsMalformed) {
  TempDir tmp;
  {
    JsonRangeLogger log(Cfg(tmp.path()));
    log.append(MakeRecord(0));
  }
  {
    std::ofstream out(tmp.path() / "scanned_ranges.jsonl", std::ios::app);
    out << "\n\n";
  }
  JsonRangeLogger log(Cfg(tmp.path()));
  EXPECT_EQ(log.size(), 1u);
  EXPECT_EQ(log.loadReport().malformed_lines_skipped, 0u);
}

TEST(JsonRangeLoggerRecovery, UnknownExtraFieldsAreTolerated) {
  TempDir tmp;
  json j = {{"start_hex", MakeRecord(0).start.ToHex()}, {"end_hex", MakeRecord(0).end.ToHex()},
            {"requested_keys", 1000}, {"scanned_keys", 1000},
            {"timestamp", "2026-10-30T15:45:00Z"}, {"status", "completed"},
            {"gpu_id", 0}, {"endomorphism_covered", false}, {"future_field", "ok"}};
  WriteRaw(tmp.path() / "scanned_ranges.jsonl", j.dump() + "\n");
  JsonRangeLogger log(Cfg(tmp.path()));
  EXPECT_EQ(log.size(), 1u);
  EXPECT_EQ(log.loadReport().malformed_lines_skipped, 0u);
}

// ----------------------------------------------------------------- concurrency

TEST(JsonRangeLoggerConcurrency, ParallelAppendsAreAllPersistedWithoutInterleaving) {
  TempDir tmp;
  constexpr int kThreads = 8, kPerThread = 100;
  {
    JsonRangeLogger log(Cfg(tmp.path(), /*flush_every_n*/ 10, /*rotate*/ 8192));
    std::atomic<int> accepted{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
      ts.emplace_back([&, t] {
        for (int i = 0; i < kPerThread; ++i)
          if (log.append(MakeRecord(static_cast<uint64_t>(t) * kPerThread + i))) ++accepted;
      });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(accepted.load(), kThreads * kPerThread);
    EXPECT_EQ(log.size(), static_cast<size_t>(kThreads * kPerThread));
  }
  size_t lines = 0;
  for (const auto& f : LoggerFiles(tmp.path()))
    for (const auto& l : ReadLines(f)) {
      ++lines;
      EXPECT_NO_THROW((void)json::parse(l)) << l;
    }
  EXPECT_EQ(lines, static_cast<size_t>(kThreads * kPerThread));

  JsonRangeLogger reopened(Cfg(tmp.path(), 10, 8192));
  EXPECT_EQ(reopened.size(), static_cast<size_t>(kThreads * kPerThread));
  EXPECT_EQ(reopened.loadReport().malformed_lines_skipped, 0u);
}

TEST(JsonRangeLoggerConcurrency, SameStartAppendedFromManyThreadsIsAcceptedExactlyOnce) {
  TempDir tmp;
  JsonRangeLogger log(Cfg(tmp.path()));
  std::atomic<int> accepted{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t) ts.emplace_back([&] { if (log.append(MakeRecord(7))) ++accepted; });
  for (auto& th : ts) th.join();
  EXPECT_EQ(accepted.load(), 1);
  EXPECT_EQ(log.size(), 1u);
}
