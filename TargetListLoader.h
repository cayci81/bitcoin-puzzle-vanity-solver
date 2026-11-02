#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace puzzle {

struct LoadedPrefixes {
  std::vector<std::string> hex_prefixes;
  std::vector<std::string> base58_prefixes;
};

class TargetListLoader {
 public:
  // Loads a list of full P2PKH Base58Check Bitcoin addresses (one per line).
  // Ignores empty/whitespace-only lines and comment lines starting with '#'.
  // Validates each address using Base58Check (25-byte payload, 0x00 version byte,
  // and 4-byte double-SHA256 checksum).
  // Throws std::runtime_error with the exact 1-based line number if any entry
  // is invalid, or if the file cannot be opened.
  static std::vector<std::string> loadAddresses(
      const std::filesystem::path& path);

  // Loads a list of prefixes (one per line), supporting both hex prefixes
  // (e.g. "5abc") and Base58 P2PKH prefixes (e.g. "1PWo").
  // Ignores empty/whitespace-only lines and comment lines starting with '#'.
  // Throws std::runtime_error with the exact 1-based line number if any entry
  // is invalid, or if the file cannot be opened.
  static LoadedPrefixes loadPrefixes(const std::filesystem::path& path);

  // Convenience helpers to load strictly hex prefixes or strictly Base58 prefixes.
  static std::vector<std::string> loadHexPrefixes(
      const std::filesystem::path& path);
  static std::vector<std::string> loadBase58Prefixes(
      const std::filesystem::path& path);

  // Validation helpers
  static bool isValidP2pkhAddress(const std::string& addr);
  static bool isValidBase58Prefix(const std::string& prefix);
  static bool isValidHexPrefix(const std::string& prefix);
};

}  // namespace puzzle

