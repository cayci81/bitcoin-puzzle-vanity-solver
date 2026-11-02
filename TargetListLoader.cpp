#include "TargetListLoader.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "Base58.h"
#include "hash/sha256.h"

namespace puzzle {

namespace {

const char* const kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

std::string TrimLine(const std::string& s) {
  size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    ++start;
  }
  size_t end = s.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(start, end - start);
}

std::string ToLowerHex(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

}  // namespace

bool TargetListLoader::isValidBase58Prefix(const std::string& prefix) {
  if (prefix.empty() || prefix.size() > 35) {
    return false;
  }
  for (char c : prefix) {
    if (std::strchr(kBase58Alphabet, c) == nullptr) {
      return false;
    }
  }
  return true;
}

bool TargetListLoader::isValidHexPrefix(const std::string& prefix) {
  if (prefix.empty() || prefix.size() > 40) {
    return false;
  }
  if (prefix.size() >= 2 && prefix[0] == '0' &&
      (prefix[1] == 'x' || prefix[1] == 'X')) {
    return false;
  }
  for (char c : prefix) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

bool TargetListLoader::isValidP2pkhAddress(const std::string& addr) {
  if (!isValidBase58Prefix(addr)) {
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

std::vector<std::string> TargetListLoader::loadAddresses(
    const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("TargetListLoader: cannot open address file: " +
                             path.string());
  }

  std::vector<std::string> addresses;
  std::string raw_line;
  size_t line_no = 0;

  while (std::getline(in, raw_line)) {
    ++line_no;
    std::string line = TrimLine(raw_line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (!isValidP2pkhAddress(line)) {
      throw std::runtime_error(
          "TargetListLoader: invalid Base58Check address at line " +
          std::to_string(line_no) + ": " + line);
    }
    addresses.push_back(line);
  }

  return addresses;
}

LoadedPrefixes TargetListLoader::loadPrefixes(
    const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("TargetListLoader: cannot open prefix file: " +
                             path.string());
  }

  LoadedPrefixes result;
  std::string raw_line;
  size_t line_no = 0;

  while (std::getline(in, raw_line)) {
    ++line_no;
    std::string line = TrimLine(raw_line);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    // Distinguish Base58 P2PKH prefixes (e.g. "1PWo") vs hex prefixes (e.g. "5abc"):
    // 1. If it starts with '1' and is valid Base58 containing any non-hex character,
    //    it is a Base58 vanity prefix.
    // 2. Otherwise if it is valid hex (1..40 nibbles), it is a hex prefix.
    // 3. Otherwise if it is valid Base58 (1..35 chars), it is a Base58 prefix.
    // 4. Otherwise throw std::runtime_error with the exact line number.
    const bool valid_hex = isValidHexPrefix(line);
    const bool valid_b58 = isValidBase58Prefix(line);

    if (valid_b58 && !valid_hex) {
      result.base58_prefixes.push_back(line);
    } else if (valid_hex) {
      result.hex_prefixes.push_back(ToLowerHex(line));
    } else {
      throw std::runtime_error("TargetListLoader: invalid prefix at line " +
                               std::to_string(line_no) + ": " + line);
    }
  }

  return result;
}

std::vector<std::string> TargetListLoader::loadHexPrefixes(
    const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("TargetListLoader: cannot open hex prefix file: " +
                             path.string());
  }

  std::vector<std::string> prefixes;
  std::string raw_line;
  size_t line_no = 0;

  while (std::getline(in, raw_line)) {
    ++line_no;
    std::string line = TrimLine(raw_line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (!isValidHexPrefix(line)) {
      throw std::runtime_error("TargetListLoader: invalid hex prefix at line " +
                               std::to_string(line_no) + ": " + line);
    }
    prefixes.push_back(ToLowerHex(line));
  }

  return prefixes;
}

std::vector<std::string> TargetListLoader::loadBase58Prefixes(
    const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error(
        "TargetListLoader: cannot open Base58 prefix file: " + path.string());
  }

  std::vector<std::string> prefixes;
  std::string raw_line;
  size_t line_no = 0;

  while (std::getline(in, raw_line)) {
    ++line_no;
    std::string line = TrimLine(raw_line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (!isValidBase58Prefix(line)) {
      throw std::runtime_error(
          "TargetListLoader: invalid Base58 prefix at line " +
          std::to_string(line_no) + ": " + line);
    }
    prefixes.push_back(line);
  }

  return prefixes;
}

}  // namespace puzzle

