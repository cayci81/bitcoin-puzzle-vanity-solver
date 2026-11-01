// Unit tests for puzzle::HexPrefixFilter and puzzle::AddressMatcher
// (TDD - no implementation exists yet).
//
// API contract exercised here (header "PrefixFilter.h"):
//
//   // Hex prefixes are matched against the hash160 as a hex string, nibble by nibble
//   // (first nibble = high nibble of byte 0). Mirrors --prefix-hex 5abc,6123.
//   class HexPrefixFilter {
//    public:
//     static HexPrefixFilter parse(const std::string& csv);   // throws std::invalid_argument
//     bool empty() const;                                     // true => no filter, match all
//     const std::vector<std::string>& prefixes() const;       // normalised lowercase, deduped
//     bool matchesHash160(const uint8_t h[20]) const;         // exact, any prefix length 1..40
//     // 65536-entry table for the existing VanitySearch 16-bit prefix lookup.
//     // table[prefix16(h)] != 0  <=>  h *may* match (exact for prefixes <= 4 nibbles,
//     // conservative superset for longer ones; the host re-checks with matchesHash160).
//     std::vector<uint8_t> buildGpuTable() const;
//     // TODO(step3): confirm prefix16() matches Vanity.cpp prefix_t byte order
//     static uint16_t prefix16(const uint8_t h[20]);          // same ordering VanitySearch uses
//   };
//
//   enum class MatchKind { None, Prefix, Full };
//   struct MatchResult { MatchKind kind; std::string matched_prefix; };
//
//   // Host-side Base58 verification. Independent of any hard-coded puzzle address.
//   class AddressMatcher {
//    public:
//     AddressMatcher(std::vector<std::string> base58_prefixes,
//                    std::optional<std::string> full_address);   // throws std::invalid_argument
//     bool isConfigured() const;
//     MatchResult classify(const std::string& address) const;
//   };
//
// Rules:
//  * parse(""): empty filter, matches everything (no filtering).
//  * parse() rejects: non-hex chars, "0x" prefix, empty tokens ("5,,6", ","), > 40 nibbles.
//    Whitespace around tokens is trimmed; case-insensitive.
//  * AddressMatcher is case-sensitive (Base58 is). Prefix chars must be in the Base58
//    alphabet (no 0, O, I, l). Full address wins over prefix. If several prefixes match,
//    the longest is reported.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <random>

#include "PrefixFilter.h"
#include "test_support.h"

using namespace puzzle;

namespace {

// 20-byte hash whose leading bytes are `head`, rest filled with 0xEE.
std::array<uint8_t, 20> Hash(std::initializer_list<uint8_t> head) {
  std::array<uint8_t, 20> h;
  h.fill(0xEE);
  size_t i = 0;
  for (uint8_t b : head) h[i++] = b;
  return h;
}

size_t CountSet(const std::vector<uint8_t>& table) {
  return static_cast<size_t>(
      std::count_if(table.begin(), table.end(), [](uint8_t v) { return v != 0; }));
}

const char* const kPuzzle71 = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
const char* const kGenesis = "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa";

}  // namespace

// ====================================================================== hex: parse

TEST(HexPrefixFilterParse, SinglePrefixNormalisedToLowercase) {
  auto f = HexPrefixFilter::parse("5ABC");
  ASSERT_EQ(f.prefixes().size(), 1u);
  EXPECT_EQ(f.prefixes()[0], "5abc");
  EXPECT_FALSE(f.empty());
}

TEST(HexPrefixFilterParse, MultiplePrefixesWithWhitespace) {
  auto f = HexPrefixFilter::parse(" 5abc , 6123 ");
  EXPECT_EQ(f.prefixes(), (std::vector<std::string>{"5abc", "6123"}));
}

TEST(HexPrefixFilterParse, DuplicatesAreCollapsed) {
  auto f = HexPrefixFilter::parse("5abc,5ABC,5abc");
  EXPECT_EQ(f.prefixes().size(), 1u);
}

TEST(HexPrefixFilterParse, EmptyStringMeansNoFilter) {
  auto f = HexPrefixFilter::parse("");
  EXPECT_TRUE(f.empty());
  EXPECT_TRUE(f.prefixes().empty());
}

TEST(HexPrefixFilterParse, RejectsMalformedInput) {
  EXPECT_THROW(HexPrefixFilter::parse("5abg"), std::invalid_argument);
  EXPECT_THROW(HexPrefixFilter::parse("0x5a"), std::invalid_argument);
  EXPECT_THROW(HexPrefixFilter::parse(","), std::invalid_argument);
  EXPECT_THROW(HexPrefixFilter::parse("5,,6"), std::invalid_argument);
  EXPECT_THROW(HexPrefixFilter::parse("5abc,"), std::invalid_argument);
  EXPECT_THROW(HexPrefixFilter::parse(std::string(41, 'a')), std::invalid_argument);
}

TEST(HexPrefixFilterParse, AcceptsFullLengthHash160Prefix) {
  EXPECT_NO_THROW(HexPrefixFilter::parse(std::string(40, 'a')));
}

// ==================================================================== hex: matching

TEST(HexPrefixFilterMatch, EmptyFilterMatchesEverything) {
  auto f = HexPrefixFilter::parse("");
  EXPECT_TRUE(f.matchesHash160(Hash({0x00}).data()));
  EXPECT_TRUE(f.matchesHash160(Hash({0xff, 0xff}).data()));
}

TEST(HexPrefixFilterMatch, FourNibblePrefixIsExact) {
  auto f = HexPrefixFilter::parse("5abc");
  EXPECT_TRUE(f.matchesHash160(Hash({0x5a, 0xbc}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x5a, 0xbd}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x5b, 0xbc}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0xa5, 0xbc}).data()));  // nibble order matters
}

TEST(HexPrefixFilterMatch, SingleNibbleMatchesHighNibbleOfFirstByte) {
  auto f = HexPrefixFilter::parse("5");
  EXPECT_TRUE(f.matchesHash160(Hash({0x50}).data()));
  EXPECT_TRUE(f.matchesHash160(Hash({0x5f}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x4f}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x60}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x05}).data()));
}

TEST(HexPrefixFilterMatch, OddLengthPrefixMatchesHalfByte) {
  auto f = HexPrefixFilter::parse("5ab");
  EXPECT_TRUE(f.matchesHash160(Hash({0x5a, 0xb0}).data()));
  EXPECT_TRUE(f.matchesHash160(Hash({0x5a, 0xbf}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x5a, 0xc0}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x5a, 0xaf}).data()));
}

TEST(HexPrefixFilterMatch, PrefixLongerThanSixteenBitsIsCheckedExactlyOnHost) {
  auto f = HexPrefixFilter::parse("5abc1");
  EXPECT_TRUE(f.matchesHash160(Hash({0x5a, 0xbc, 0x10}).data()));
  EXPECT_TRUE(f.matchesHash160(Hash({0x5a, 0xbc, 0x1f}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x5a, 0xbc, 0x20}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x5a, 0xbc, 0x01}).data()));
}

TEST(HexPrefixFilterMatch, FullFortyNibblePrefixMatchesOnlyThatHash) {
  auto h = Hash({0x01, 0x02, 0x03});
  std::string hex = "010203";
  hex += std::string(34, 'e');
  auto f = HexPrefixFilter::parse(hex);
  EXPECT_TRUE(f.matchesHash160(h.data()));
  h[19] = 0xEF;
  EXPECT_FALSE(f.matchesHash160(h.data()));
}

TEST(HexPrefixFilterMatch, AnyOfMultiplePrefixesMatches) {
  auto f = HexPrefixFilter::parse("5abc,6123");
  EXPECT_TRUE(f.matchesHash160(Hash({0x5a, 0xbc}).data()));
  EXPECT_TRUE(f.matchesHash160(Hash({0x61, 0x23}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x5a, 0x12}).data()));
  EXPECT_FALSE(f.matchesHash160(Hash({0x61, 0xbc}).data()));
}

// ================================================================ hex: GPU table

TEST(HexPrefixFilterGpuTable, HasSixtyFiveThousandFiveHundredThirtySixEntries) {
  EXPECT_EQ(HexPrefixFilter::parse("5abc").buildGpuTable().size(), 65536u);
}

TEST(HexPrefixFilterGpuTable, EntryCountDependsOnPrefixLength) {
  EXPECT_EQ(CountSet(HexPrefixFilter::parse("5").buildGpuTable()), 4096u);
  EXPECT_EQ(CountSet(HexPrefixFilter::parse("5a").buildGpuTable()), 256u);
  EXPECT_EQ(CountSet(HexPrefixFilter::parse("5ab").buildGpuTable()), 16u);
  EXPECT_EQ(CountSet(HexPrefixFilter::parse("5abc").buildGpuTable()), 1u);
}

TEST(HexPrefixFilterGpuTable, LongerThanFourNibblesCollapsesToItsFirstSixteenBits) {
  auto t = HexPrefixFilter::parse("5abc1").buildGpuTable();
  EXPECT_EQ(CountSet(t), 1u);
  EXPECT_NE(t[HexPrefixFilter::prefix16(Hash({0x5a, 0xbc, 0x99}).data())], 0);
}

TEST(HexPrefixFilterGpuTable, MultiplePrefixesAreUnionedAndOverlapIsDeduplicated) {
  EXPECT_EQ(CountSet(HexPrefixFilter::parse("5abc,6123").buildGpuTable()), 2u);
  EXPECT_EQ(CountSet(HexPrefixFilter::parse("5,5a").buildGpuTable()), 4096u);
}

TEST(HexPrefixFilterGpuTable, EmptyFilterSetsEveryEntry) {
  EXPECT_EQ(CountSet(HexPrefixFilter::parse("").buildGpuTable()), 65536u);
}

// Verified against Vanity.cpp:505, GPUCompute.h:54, GPUEngine.cu:769
// The index is little-endian: h[0] | (h[1] << 8).
TEST(HexPrefixFilterGpuTable, IndexedByPrefix16) {
  auto t = HexPrefixFilter::parse("5abc").buildGpuTable();
  EXPECT_NE(t[HexPrefixFilter::prefix16(Hash({0x5a, 0xbc}).data())], 0);
  EXPECT_EQ(t[HexPrefixFilter::prefix16(Hash({0x5a, 0xbd}).data())], 0);
  EXPECT_EQ(t[HexPrefixFilter::prefix16(Hash({0xbc, 0x5a}).data())], 0);
}

TEST(HexPrefixFilterGpuTable, NeverProducesFalseNegativesAndIsExactUpToFourNibbles) {
  std::mt19937 rng(12345);
  for (const char* csv : {"5", "5a", "5ab", "5abc", "5abc,6123", "5abc1", "7,c0f"}) {
    auto f = HexPrefixFilter::parse(csv);
    auto table = f.buildGpuTable();
    const bool exact = [&] {
      for (const auto& p : f.prefixes()) if (p.size() > 4) return false;
      return true;
    }();
    for (int i = 0; i < 20000; ++i) {
      std::array<uint8_t, 20> h;
      for (auto& b : h) b = static_cast<uint8_t>(rng());
      const bool host = f.matchesHash160(h.data());
      const bool gpu = table[HexPrefixFilter::prefix16(h.data())] != 0;
      if (host) EXPECT_TRUE(gpu) << "false negative for prefix set " << csv;  // fatal for a solver
      if (exact) EXPECT_EQ(host, gpu) << csv;
    }
  }
}

// ======================================================== base58 / full address

TEST(AddressMatcher, FullAddressExactMatch) {
  AddressMatcher m({}, std::string(kPuzzle71));
  auto r = m.classify(kPuzzle71);
  EXPECT_EQ(r.kind, MatchKind::Full);
}

TEST(AddressMatcher, DifferentAddressDoesNotMatchFull) {
  AddressMatcher m({}, std::string(kPuzzle71));
  EXPECT_EQ(m.classify(kGenesis).kind, MatchKind::None);
  EXPECT_EQ(m.classify("1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXV").kind, MatchKind::None);  // last char
}

TEST(AddressMatcher, FullAddressIsConfigurableForOtherPuzzles) {
  AddressMatcher m({}, std::string(kGenesis));
  EXPECT_EQ(m.classify(kGenesis).kind, MatchKind::Full);
  EXPECT_EQ(m.classify(kPuzzle71).kind, MatchKind::None);
}

TEST(AddressMatcher, Base58PrefixMatch) {
  AddressMatcher m({"1PWo3JeB9"}, std::nullopt);
  auto r = m.classify(kPuzzle71);
  EXPECT_EQ(r.kind, MatchKind::Prefix);
  EXPECT_EQ(r.matched_prefix, "1PWo3JeB9");
  EXPECT_EQ(m.classify(kGenesis).kind, MatchKind::None);
}

TEST(AddressMatcher, ShortPrefixMatches) {
  AddressMatcher m({"1PW"}, std::nullopt);
  EXPECT_EQ(m.classify(kPuzzle71).kind, MatchKind::Prefix);
}

TEST(AddressMatcher, PrefixIsCaseSensitive) {
  AddressMatcher m({"1pwo3"}, std::nullopt);
  EXPECT_EQ(m.classify(kPuzzle71).kind, MatchKind::None);
}

TEST(AddressMatcher, PrefixDivergingAtLastCharDoesNotMatch) {
  AddressMatcher m({"1PWo3JeB8"}, std::nullopt);
  EXPECT_EQ(m.classify(kPuzzle71).kind, MatchKind::None);
}

TEST(AddressMatcher, PrefixLongerThanAddressDoesNotMatch) {
  AddressMatcher m({std::string(kPuzzle71) + "x"}, std::nullopt);  // 'x' is valid Base58
  EXPECT_EQ(m.classify(kPuzzle71).kind, MatchKind::None);
}

TEST(AddressMatcher, AnyOfMultiplePrefixesMatchesAndLongestIsReported) {
  AddressMatcher m({"1A1", "1PW", "1PWo3J"}, std::nullopt);
  auto r = m.classify(kPuzzle71);
  EXPECT_EQ(r.kind, MatchKind::Prefix);
  EXPECT_EQ(r.matched_prefix, "1PWo3J");
  EXPECT_EQ(m.classify(kGenesis).kind, MatchKind::Prefix);
}

TEST(AddressMatcher, FullMatchTakesPriorityOverPrefixMatch) {
  AddressMatcher m({"1PWo3"}, std::string(kPuzzle71));
  EXPECT_EQ(m.classify(kPuzzle71).kind, MatchKind::Full);
  EXPECT_EQ(m.classify("1PWo3AAAAAAAAAAAAAAAAAAAAAAAAAAAAA").kind, MatchKind::Prefix);
}

TEST(AddressMatcher, UnconfiguredMatcherReportsNoMatch) {
  AddressMatcher m({}, std::nullopt);
  EXPECT_FALSE(m.isConfigured());
  EXPECT_EQ(m.classify(kPuzzle71).kind, MatchKind::None);
}

TEST(AddressMatcher, RejectsCharactersOutsideBase58Alphabet) {
  EXPECT_THROW(AddressMatcher({"1PWo0"}, std::nullopt), std::invalid_argument);  // '0'
  EXPECT_THROW(AddressMatcher({"1PWoO"}, std::nullopt), std::invalid_argument);  // 'O'
  EXPECT_THROW(AddressMatcher({"1PWoI"}, std::nullopt), std::invalid_argument);  // 'I'
  EXPECT_THROW(AddressMatcher({"1PWol"}, std::nullopt), std::invalid_argument);  // 'l'
  EXPECT_THROW(AddressMatcher({""}, std::nullopt), std::invalid_argument);       // empty prefix
  EXPECT_THROW(AddressMatcher({}, std::string("1PWo0JeB9jrGwfHDNpdGK54CRas7fsVzXU")),
               std::invalid_argument);
}
