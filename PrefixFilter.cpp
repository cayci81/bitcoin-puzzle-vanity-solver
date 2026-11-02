#include "PrefixFilter.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "Base58.h"
#include "hash/sha256.h"

namespace puzzle {

namespace {

const char* const kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

bool IsValidBase58String(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (std::strchr(kBase58Alphabet, c) == nullptr) {
      return false;
    }
  }
  return true;
}

bool IsValidP2pkhAddress(const std::string& addr) {
  if (!IsValidBase58String(addr)) {
    return false;
  }
  std::vector<unsigned char> decoded;
  if (!DecodeBase58(addr, decoded) || decoded.size() != 25) {
    return false;
  }
  if (decoded[0] != 0x00) {
    return false;
  }
  uint8_t expected_checksum[4] = {0};
  sha256_checksum(decoded.data(), 21, expected_checksum);
  return std::memcmp(expected_checksum, decoded.data() + 21, 4) == 0;
}

std::string Trim(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
    start++;
  }
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }
  return s.substr(start, end - start);
}

uint8_t HexCharToNibble(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
  throw std::invalid_argument("Invalid hex character");
}

}  // namespace

HexPrefixFilter HexPrefixFilter::parse(const std::vector<std::string>& prefixes) {
  HexPrefixFilter filter;
  for (const auto& token : prefixes) {
    std::string trimmed = Trim(token);
    if (trimmed.empty()) {
      throw std::invalid_argument("HexPrefixFilter: empty token");
    }

    // Reject "0x" or "0X" prefix
    if (trimmed.size() >= 2 && trimmed[0] == '0' &&
        (trimmed[1] == 'x' || trimmed[1] == 'X')) {
      throw std::invalid_argument("HexPrefixFilter: '0x' prefix not allowed");
    }

    if (trimmed.size() > 40) {
      throw std::invalid_argument("HexPrefixFilter: prefix exceeds 40 nibbles (20 bytes)");
    }

    std::string lower;
    lower.reserve(trimmed.size());
    for (char c : trimmed) {
      if (!std::isxdigit(static_cast<unsigned char>(c))) {
        throw std::invalid_argument("HexPrefixFilter: non-hex character in token");
      }
      lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    // Deduplicate and pre-decode into raw byte + mask arrays for fast hot-path matching
    if (std::find(filter.prefixes_.begin(), filter.prefixes_.end(), lower) ==
        filter.prefixes_.end()) {
      DecodedPrefix dp;
      dp.byte_len = (lower.size() + 1) / 2;
      for (size_t i = 0; i < lower.size(); ++i) {
        const size_t byte_idx = i / 2;
        const uint8_t nibble = HexCharToNibble(lower[i]);
        if (i % 2 == 0) {
          dp.bytes[byte_idx] |= static_cast<uint8_t>(nibble << 4);
          dp.mask[byte_idx] |= 0xF0u;
        } else {
          dp.bytes[byte_idx] |= nibble;
          dp.mask[byte_idx] |= 0x0Fu;
        }
      }
      filter.prefixes_.push_back(lower);
      filter.decoded_prefixes_.push_back(dp);
    }
  }
  return filter;
}

HexPrefixFilter HexPrefixFilter::parse(const std::string& csv) {
  if (csv.empty()) {
    return HexPrefixFilter{};
  }

  // Comma-separated token parser
  // Reject leading comma, trailing comma, or empty consecutive tokens
  if (csv.front() == ',' || csv.back() == ',') {
    throw std::invalid_argument("HexPrefixFilter: empty token around comma");
  }

  std::vector<std::string> tokens;
  std::stringstream ss(csv);
  std::string token;
  while (std::getline(ss, token, ',')) {
    tokens.push_back(token);
  }

  return parse(tokens);
}

bool HexPrefixFilter::empty() const {
  return prefixes_.empty();
}

const std::vector<std::string>& HexPrefixFilter::prefixes() const {
  return prefixes_;
}

bool HexPrefixFilter::matchesHash160(const uint8_t h[20]) const {
  if (decoded_prefixes_.empty()) {
    return true;
  }

  for (const auto& dp : decoded_prefixes_) {
    bool match = true;
    for (size_t i = 0; i < dp.byte_len; ++i) {
      if ((h[i] & dp.mask[i]) != dp.bytes[i]) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }

  return false;
}

std::vector<uint8_t> HexPrefixFilter::buildGpuTable() const {
  std::vector<uint8_t> table(65536, 0);
  if (empty()) {
    std::fill(table.begin(), table.end(), static_cast<uint8_t>(1));
    return table;
  }

  for (const auto& p : prefixes_) {
    if (p.size() == 1) {
      uint8_t n0 = HexCharToNibble(p[0]);
      for (uint32_t lo = 0; lo < 16; ++lo) {
        uint8_t b0 = static_cast<uint8_t>((n0 << 4) | lo);
        for (uint32_t b1 = 0; b1 < 256; ++b1) {
          table[b0 | (b1 << 8)] = 1;
        }
      }
    } else if (p.size() == 2) {
      uint8_t b0 = static_cast<uint8_t>((HexCharToNibble(p[0]) << 4) | HexCharToNibble(p[1]));
      for (uint32_t b1 = 0; b1 < 256; ++b1) {
        table[b0 | (b1 << 8)] = 1;
      }
    } else if (p.size() == 3) {
      uint8_t b0 = static_cast<uint8_t>((HexCharToNibble(p[0]) << 4) | HexCharToNibble(p[1]));
      uint8_t n2 = HexCharToNibble(p[2]);
      for (uint32_t lo = 0; lo < 16; ++lo) {
        uint8_t b1 = static_cast<uint8_t>((n2 << 4) | lo);
        table[b0 | (b1 << 8)] = 1;
      }
    } else {
      // p.size() >= 4: first 4 nibbles fix both byte 0 and byte 1
      uint8_t b0 = static_cast<uint8_t>((HexCharToNibble(p[0]) << 4) | HexCharToNibble(p[1]));
      uint8_t b1 = static_cast<uint8_t>((HexCharToNibble(p[2]) << 4) | HexCharToNibble(p[3]));
      table[b0 | (b1 << 8)] = 1;
    }
  }

  return table;
}

uint16_t HexPrefixFilter::prefix16(const uint8_t h[20]) {
  // Verified against Vanity.cpp:505, GPUCompute.h:54, GPUEngine.cu:769
  // Little-endian first 2 bytes: h[0] | (h[1] << 8)
  return static_cast<uint16_t>(h[0] | (static_cast<uint16_t>(h[1]) << 8));
}

// ------------------------------------------------------------- AddressMatcher

AddressMatcher::AddressMatcher(std::vector<std::string> base58_prefixes,
                               std::optional<std::string> full_address)
    : AddressMatcher(
          std::move(base58_prefixes),
          full_address.has_value()
              ? std::vector<std::string>{*std::move(full_address)}
              : std::vector<std::string>{}) {}

AddressMatcher::AddressMatcher(std::vector<std::string> base58_prefixes,
                               std::vector<std::string> full_addresses)
    : prefixes_(std::move(base58_prefixes)),
      full_addresses_(std::move(full_addresses)) {
  for (const auto& p : prefixes_) {
    if (!IsValidBase58String(p)) {
      throw std::invalid_argument("AddressMatcher: invalid Base58 prefix");
    }
  }

  for (const auto& addr : full_addresses_) {
    if (!IsValidP2pkhAddress(addr)) {
      throw std::invalid_argument(
          "AddressMatcher: invalid Base58Check P2PKH full address");
    }
  }
}

bool AddressMatcher::isConfigured() const {
  return !full_addresses_.empty() || !prefixes_.empty();
}

MatchResult AddressMatcher::classify(const std::string& address) const {
  // 1. Full address match takes highest priority
  for (const auto& full_addr : full_addresses_) {
    if (address == full_addr) {
      return MatchResult{MatchKind::Full, full_addr};
    }
  }

  // 2. Base58 prefix match: longest matching prefix wins
  std::string longest_match;
  for (const auto& p : prefixes_) {
    if (address.rfind(p, 0) == 0) {
      if (p.size() > longest_match.size()) {
        longest_match = p;
      }
    }
  }

  if (!longest_match.empty()) {
    return MatchResult{MatchKind::Prefix, longest_match};
  }

  return MatchResult{MatchKind::None, ""};
}

}  // namespace puzzle
