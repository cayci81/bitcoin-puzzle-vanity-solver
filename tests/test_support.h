// Shared helpers for the Puzzle #71 unit tests.
// Test-only: nothing here ships in the production binaries.
//
// Contract assumed from the production header "RangeKey.h" (thin wrapper around
// the existing VanitySearch `Int` class; NO new big-integer type is introduced):
//
//   namespace puzzle {
//   struct RangeKey {
//     Int value;                                        // reused VanitySearch Int
//     static RangeKey FromHex(const std::string&);      // accepts upper/lower case, no "0x"
//     std::string     ToHex() const;                    // lowercase, no prefix, no leading zeros
//     RangeKey        plus(uint64_t n) const;           // value + n
//     Int             distanceTo(const RangeKey& o) const; // o - value (precondition o >= value)
//     bool operator==(const RangeKey&) const;
//     bool operator< (const RangeKey&) const;
//     bool operator<=(const RangeKey&) const;
//   };
//   }  // std::hash<puzzle::RangeKey> is specialised (hashes bits64[0..3]).
//
// Why a wrapper at all: `Int` exposes a pointer-based, non-const API and has no
// operator==/operator</std::hash, all of which are required for std::sort and
// std::unordered_set<RangeKey>. The wrapper adds those and nothing else.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Int.h"
#include "Random.h"
#include "RangeKey.h"

namespace puzzle::test {

namespace fs = std::filesystem;

inline RangeKey RK(const std::string& hex) { return RangeKey::FromHex(hex); }

// Low 64 bits of an Int; asserts the value really fits (small-range tests only).
inline uint64_t U64(Int v) {
  for (int i = 1; i < NB64BLOCK; ++i) {
    EXPECT_EQ(v.bits64[i], 0ULL) << "value does not fit in 64 bits";
  }
  return v.bits64[0];
}

// Offset of `k` from `base` as uint64 (small-range tests only).
inline uint64_t Offset(const RangeKey& base, const RangeKey& k) {
  return U64(base.distanceTo(k));
}

// Which eighth of [start, start + 2^70) does `k` fall into? (0..7)
// Only meaningful for the real puzzle #71 range, whose width is exactly 2^70.
inline int Bucket8(const RangeKey& start, const RangeKey& k) {
  Int off = start.distanceTo(k);
  off.ShiftR(67);  // 2^70 / 8 = 2^67
  return static_cast<int>(off.bits64[0]);
}

// RAII unique temporary directory, removed on destruction.
class TempDir {
 public:
  TempDir() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::random_device rd;
    path_ = fs::temp_directory_path() /
            ("puzzle71_" + std::string(info->test_suite_name()) + "_" +
             std::string(info->name()) + "_" + std::to_string(rd()));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

inline std::vector<std::string> ReadLines(const fs::path& p) {
  std::vector<std::string> lines;
  std::ifstream in(p, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  return lines;
}

inline void WriteRaw(const fs::path& p, const std::string& content) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << content;
}

}  // namespace puzzle::test
