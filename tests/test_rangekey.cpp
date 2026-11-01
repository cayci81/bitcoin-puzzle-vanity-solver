#include <gtest/gtest.h>

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "RangeKey.h"
#include "test_support.h"

using namespace puzzle;
using namespace puzzle::test;

// 1. FromHex & ToHex round trips and conversions
TEST(RangeKeyTest, RoundTripPuzzleBounds) {
  const std::string start_hex = "400000000000000000";
  const std::string end_hex = "7fffffffffffffffff";

  RangeKey start = RangeKey::FromHex(start_hex);
  EXPECT_EQ(start.ToHex(), start_hex);

  RangeKey end = RangeKey::FromHex(end_hex);
  EXPECT_EQ(end.ToHex(), end_hex);
}

TEST(RangeKeyTest, FromHexOddLengthLeftPad) {
  RangeKey k = RangeKey::FromHex("abc");
  EXPECT_EQ(k.ToHex(), "abc");
}

TEST(RangeKeyTest, FromHexUppercaseNormalizedToLowercase) {
  RangeKey k = RangeKey::FromHex("ABCDEF");
  EXPECT_EQ(k.ToHex(), "abcdef");
}

TEST(RangeKeyTest, ToHexZeroIsZero) {
  RangeKey k(0ULL);
  EXPECT_EQ(k.ToHex(), "0");

  RangeKey k_default;
  EXPECT_EQ(k_default.ToHex(), "0");
}

// 2. FromHex validation and exceptions
TEST(RangeKeyTest, FromHexRejectsEmptyString) {
  EXPECT_THROW(RangeKey::FromHex(""), std::invalid_argument);
}

TEST(RangeKeyTest, FromHexRejects0xPrefix) {
  EXPECT_THROW(RangeKey::FromHex("0x1234"), std::invalid_argument);
  EXPECT_THROW(RangeKey::FromHex("0X1234"), std::invalid_argument);
}

TEST(RangeKeyTest, FromHexRejectsNonHexCharacters) {
  EXPECT_THROW(RangeKey::FromHex("12g4"), std::invalid_argument);
  EXPECT_THROW(RangeKey::FromHex("1234 "), std::invalid_argument);
  EXPECT_THROW(RangeKey::FromHex(" 1234"), std::invalid_argument);
  EXPECT_THROW(RangeKey::FromHex("12 34"), std::invalid_argument);
}

TEST(RangeKeyTest, FromHexRejectsExceeding256Bits) {
  EXPECT_THROW(RangeKey::FromHex(std::string(65, 'f')), std::invalid_argument);
  EXPECT_NO_THROW(RangeKey::FromHex(std::string(64, 'f')));
}

// 3. Arithmetic: plus and distanceTo
// Regression guard: initial test draft had an off-by-one zero count in expected
// hex ("40000001..." with 7 zeros instead of 8 zeros "4000000001...").
// In addition, if WIN64 is undefined on MSVC, Int.h routes through the GCC
// __asm__ / __builtin path, breaking 64-bit carry and 2^32 arithmetic.
TEST(RangeKeyTest, PlusRoundTrip) {
  RangeKey base = RangeKey::FromHex("400000000000000000");

  RangeKey p1 = base.plus(1);
  EXPECT_EQ(p1.ToHex(), "400000000000000001");

  RangeKey p_high = base.plus(0x100000000ULL);
  EXPECT_EQ(p_high.ToHex(), "400000000100000000");
}

TEST(RangeKeyTest, DistanceToConsistency) {
  RangeKey a = RangeKey::FromHex("400000000000000000");
  const uint64_t deltas[] = {0, 1, 255, 0xFFFFFFFFULL};

  for (uint64_t n : deltas) {
    RangeKey b = a.plus(n);
    Int dist = a.distanceTo(b);
    EXPECT_EQ(U64(dist), n) << "Failed for n = " << n;
  }
}

// 4. Comparison Operators & Strict Total Order
TEST(RangeKeyTest, StrictTotalOrder) {
  RangeKey a = RangeKey::FromHex("1000");
  RangeKey b = RangeKey::FromHex("2000");
  RangeKey c = RangeKey::FromHex("3000");

  // Asymmetry: a < b implies !(b < a)
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);

  // Irreflexivity: !(a < a)
  EXPECT_FALSE(a < a);

  // Transitivity: a < b and b < c implies a < c
  EXPECT_TRUE(b < c);
  EXPECT_TRUE(a < c);

  // Equality
  RangeKey a_copy = RangeKey::FromHex("1000");
  EXPECT_TRUE(a == a_copy);
  EXPECT_FALSE(a != a_copy);
  EXPECT_FALSE(a < a_copy);
  EXPECT_FALSE(a_copy < a);
}

TEST(RangeKeyTest, ComparisonConsistency) {
  RangeKey lo = RangeKey::FromHex("100");
  RangeKey hi = RangeKey::FromHex("200");
  RangeKey eq = RangeKey::FromHex("100");

  EXPECT_TRUE(lo <= hi);
  EXPECT_TRUE(lo <= eq);
  EXPECT_FALSE(hi <= lo);

  EXPECT_TRUE(hi > lo);
  EXPECT_FALSE(lo > hi);
  EXPECT_FALSE(lo > eq);

  EXPECT_TRUE(hi >= lo);
  EXPECT_TRUE(lo >= eq);
  EXPECT_FALSE(lo >= hi);
}

// 5. Hash & Standard Container Compatibility
TEST(RangeKeyTest, HashConsistencyAndDistinctness) {
  RangeKey a = RangeKey::FromHex("400000000000000000");
  RangeKey a_same = RangeKey::FromHex("400000000000000000");
  std::hash<RangeKey> hasher;

  EXPECT_EQ(hasher(a), hasher(a_same));

  // 10 distinct keys must produce 10 distinct hashes
  std::set<size_t> hashes;
  for (uint64_t i = 0; i < 10; ++i) {
    RangeKey k = a.plus(i * 1000 + 1);
    hashes.insert(hasher(k));
  }
  EXPECT_EQ(hashes.size(), 10u);
}

TEST(RangeKeyTest, UnorderedSetIntegration) {
  std::unordered_set<RangeKey> set;
  RangeKey k1 = RangeKey::FromHex("100");
  RangeKey k2 = RangeKey::FromHex("200");
  RangeKey k3 = RangeKey::FromHex("300");

  set.insert(k1);
  set.insert(k2);

  EXPECT_EQ(set.size(), 2u);
  EXPECT_NE(set.find(k1), set.end());
  EXPECT_NE(set.find(k2), set.end());
  EXPECT_EQ(set.find(k3), set.end());

  // Duplicate insert
  auto [iter, inserted] = set.insert(RangeKey::FromHex("100"));
  EXPECT_FALSE(inserted);
  EXPECT_EQ(set.size(), 2u);
}

TEST(RangeKeyTest, MapOrderingIntegration) {
  std::map<RangeKey, std::string> map;
  RangeKey k3 = RangeKey::FromHex("300");
  RangeKey k1 = RangeKey::FromHex("100");
  RangeKey k2 = RangeKey::FromHex("200");

  map[k3] = "three";
  map[k1] = "one";
  map[k2] = "two";

  std::vector<std::string> ordered_values;
  for (const auto& [key, val] : map) {
    ordered_values.push_back(val);
  }

  // Map iterates in ascending operator< order
  EXPECT_EQ(ordered_values, (std::vector<std::string>{"one", "two", "three"}));
}
