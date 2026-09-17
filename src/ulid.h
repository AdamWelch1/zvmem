#pragma once

#include <chrono>
#include <cstdint>
#include <random>
#include <string>

namespace zvmem {

// ULID: 48-bit big-endian millisecond timestamp + 80 random bits, encoded as 26
// Crockford base32 characters. Lexicographically sortable by creation time.
inline std::string new_ulid() {
  static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

  uint64_t ts = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  uint8_t b[16] = {0};
  for (int i = 5; i >= 0; --i) b[i] = static_cast<uint8_t>((ts >> (8 * i)) & 0xFF);
  std::random_device rd;
  for (int i = 6; i < 16; ++i) b[i] = static_cast<uint8_t>(rd() & 0xFF);

  std::string out;
  out.reserve(26);
  for (int i = 0; i < 26; ++i) {
    int m = 5 * i;          // bit index from the MSB of the 128-bit value
    int byte_idx = m / 8;
    int off = m % 8;        // bits consumed from the top of b[byte_idx]
    uint32_t val = b[byte_idx] >> off;  // (8 - off) most-significant bits
    if (off > 3) {          // need (off - 3) more bits from the next byte
      int need = off - 3;
      val = (val << need) | (b[byte_idx + 1] >> (8 - need));
    }
    out.push_back(kAlphabet[val & 0x1F]);
  }
  return out;
}

}  // namespace zvmem
