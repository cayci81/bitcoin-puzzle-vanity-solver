#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include "Int.h"

namespace puzzle {

// Thin wrapper around VanitySearch's 256-bit `Int` class.
// Provides const correctness, comparison operators, and hashing
// without adding any new big-integer arithmetic.
struct RangeKey {
  Int value;

  RangeKey();
  explicit RangeKey(const Int& v);
  explicit RangeKey(uint64_t u64);

  // Parsing & formatting
  // FromHex accepts uppercase/lowercase hex strings (without "0x" prefix).
  static RangeKey FromHex(const std::string& hex);
  // ToHex returns lowercase hex without "0x" and without leading zeros (unless zero).
  std::string ToHex() const;

  // Arithmetic helpers delegating to Int
  // plus(uint64_t) — n must fit in uint64_t; callers adding larger
  // offsets should compose multiple calls.
  RangeKey plus(uint64_t n) const;
  // o - value (precondition: o >= *this. Precondition violation → undefined (debug: assert).)
  Int distanceTo(const RangeKey& o) const;

  // Comparison operators
  bool operator==(const RangeKey& o) const;
  bool operator!=(const RangeKey& o) const;
  bool operator<(const RangeKey& o) const;
  bool operator<=(const RangeKey& o) const;
  bool operator>(const RangeKey& o) const;
  bool operator>=(const RangeKey& o) const;
};

}  // namespace puzzle

namespace std {
template <>
struct hash<puzzle::RangeKey> {
  size_t operator()(const puzzle::RangeKey& k) const noexcept;
};
}  // namespace std
