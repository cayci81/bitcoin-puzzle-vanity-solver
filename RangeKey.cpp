#include "RangeKey.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace puzzle {

RangeKey::RangeKey() {
  for (int i = 0; i < NB64BLOCK; ++i) {
    value.bits64[i] = 0;
  }
}

RangeKey::RangeKey(const Int& v) {
  for (int i = 0; i < NB64BLOCK; ++i) {
    value.bits64[i] = v.bits64[i];
  }
}

RangeKey::RangeKey(uint64_t u64) : value(u64) {}

RangeKey RangeKey::FromHex(const std::string& hex) {
  if (hex.empty()) {
    throw std::invalid_argument("RangeKey::FromHex: empty hex string");
  }

  // Reject 0x / 0X prefix explicitly
  if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
    throw std::invalid_argument("RangeKey::FromHex: '0x' prefix not allowed");
  }

  // Validate characters
  for (char c : hex) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      throw std::invalid_argument("RangeKey::FromHex: invalid hex character");
    }
  }

  // Max 256 bits (64 hex digits). NB64BLOCK is 5 (4 limbs + 1 Knuth limb)
  if (hex.size() > 64) {
    throw std::invalid_argument("RangeKey::FromHex: hex string exceeds 256 bits");
  }

  // If odd length, left-pad with a single zero
  std::string s = (hex.size() % 2 != 0) ? ("0" + hex) : hex;

  RangeKey r;
  size_t end = s.size();
  int limb = 0;
  while (end > 0 && limb < NB64BLOCK) {
    size_t chunk_len = (end >= 16) ? 16 : end;
    size_t chunk_start = end - chunk_len;
    uint64_t val = 0;
    for (size_t i = 0; i < chunk_len; ++i) {
      char c = s[chunk_start + i];
      uint8_t d = 0;
      if (c >= '0' && c <= '9') {
        d = static_cast<uint8_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        d = static_cast<uint8_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        d = static_cast<uint8_t>(c - 'A' + 10);
      }
      val = (val << 4) | d;
    }
    r.value.bits64[limb++] = val;
    end = chunk_start;
  }

  return r;
}

std::string RangeKey::ToHex() const {
  int highest = NB64BLOCK - 1;
  while (highest > 0 && value.bits64[highest] == 0) {
    highest--;
  }
  if (highest == 0 && value.bits64[0] == 0) {
    return "0";
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(value.bits64[highest]));
  std::string s = buf;

  for (int i = highest - 1; i >= 0; --i) {
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value.bits64[i]));
    s += buf;
  }
  return s;
}

RangeKey RangeKey::plus(uint64_t n) const {
  RangeKey r(*this);
  r.value.Add(n);
  if (r.value.bits64[4] != 0) {
    throw std::overflow_error("RangeKey::plus: 256-bit overflow");
  }
  return r;
}

Int RangeKey::distanceTo(const RangeKey& o) const {
  if (o < *this) {
    throw std::invalid_argument("RangeKey::distanceTo: target key is less than start key");
  }
  Int diff;
  // const_cast is required because Int::Sub takes Int* (not const Int*);
  // Sub only reads from its arguments so casting away const is safe.
  diff.Sub(const_cast<Int*>(&o.value), const_cast<Int*>(&value));
  return diff;
}

bool RangeKey::operator==(const RangeKey& o) const {
  for (int i = 0; i < NB64BLOCK; ++i) {
    if (value.bits64[i] != o.value.bits64[i]) return false;
  }
  return true;
}

bool RangeKey::operator!=(const RangeKey& o) const {
  return !(*this == o);
}

bool RangeKey::operator<(const RangeKey& o) const {
  for (int i = NB64BLOCK - 1; i >= 0; --i) {
    if (value.bits64[i] < o.value.bits64[i]) return true;
    if (value.bits64[i] > o.value.bits64[i]) return false;
  }
  return false;
}

bool RangeKey::operator<=(const RangeKey& o) const {
  return (*this < o) || (*this == o);
}

bool RangeKey::operator>(const RangeKey& o) const {
  return o < *this;
}

bool RangeKey::operator>=(const RangeKey& o) const {
  return !(*this < o);
}

}  // namespace puzzle

namespace std {
size_t hash<puzzle::RangeKey>::operator()(const puzzle::RangeKey& k) const noexcept {
  uint64_t h = 14695981039346656037ull;
  for (int i = 0; i < NB64BLOCK; ++i) {
    h ^= k.value.bits64[i];
    h *= 1099511628211ull;
  }
  return static_cast<size_t>(h);
}
}  // namespace std
