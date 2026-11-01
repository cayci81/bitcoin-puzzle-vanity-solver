#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace puzzle {

// GPU & Host hex prefix filter (matching raw 20-byte hash160).
// Configured via CLI: e.g. --prefix-hex 5abc,6123
class HexPrefixFilter {
 public:
  // Parses comma-separated hex prefixes.
  // Throws std::invalid_argument on non-hex characters, empty tokens, "0x" prefix, or length > 40.
  // parse("") returns an empty filter that matches all hashes.
  static HexPrefixFilter parse(const std::string& csv);

  bool empty() const;
  const std::vector<std::string>& prefixes() const;

  // Exact match against raw 20-byte hash160 for arbitrary prefix lengths (1..40 nibbles).
  bool matchesHash160(const uint8_t h[20]) const;

  // Builds a 65536-entry (64 KB) lookup table for the VanitySearch GPU kernel.
  // Indexed by prefix16(h).
  // table[prefix16(h)] != 0 if h could match.
  // Exact for prefixes <= 4 nibbles, conservative superset for longer prefixes.
  std::vector<uint8_t> buildGpuTable() const;

  // Verified against Vanity.cpp:505, GPUCompute.h:54, GPUEngine.cu:769
  // The index is little-endian: h[0] | (h[1] << 8).
  static uint16_t prefix16(const uint8_t h[20]);

 private:
  struct DecodedPrefix {
    std::array<uint8_t, 20> bytes{};
    std::array<uint8_t, 20> mask{};
    size_t byte_len = 0;
  };

  std::vector<std::string> prefixes_;
  std::vector<DecodedPrefix> decoded_prefixes_;
};

enum class MatchKind {
  None,
  Prefix,
  Full
};

struct MatchResult {
  MatchKind kind = MatchKind::None;
  std::string matched_prefix;
};

// Host CPU Base58 address matcher.
// Matches against optional full target address and/or list of Base58 prefixes.
// Fully configurable and independent of any hard-coded address.
class AddressMatcher {
 public:
  // Throws std::invalid_argument if any prefix or address contains invalid Base58 characters
  // (e.g. '0', 'O', 'I', 'l') or if a prefix is empty.
  AddressMatcher(std::vector<std::string> base58_prefixes,
                 std::optional<std::string> full_address);

  bool isConfigured() const;

  // Classifies an address:
  // Full match takes precedence over prefix matches.
  // If multiple prefixes match, the longest prefix is recorded.
  MatchResult classify(const std::string& address) const;

 private:
  std::vector<std::string> prefixes_;
  std::optional<std::string> full_address_;
};

}  // namespace puzzle
