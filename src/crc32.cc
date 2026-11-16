#include "crc32.h"

#include <array>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace lsm::crc32 {

namespace {
constexpr uint32_t kPoly = 0xEDB88320u;  // 0x04C11DB7 bit-reversed

constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (kPoly ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
  return t;
}
constexpr std::array<uint32_t, 256> kTable = MakeTable();
}  // namespace

uint32_t ExtendPortable(uint32_t crc, const char* data, size_t n) {
  uint32_t c = ~crc;
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; i++) c = kTable[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
#if defined(__ARM_FEATURE_CRC32)
  uint32_t c = ~crc;
  while (n >= 8) {
    uint64_t word;
    std::memcpy(&word, data, 8);  // memcpy avoids unaligned-access UB
    c = __crc32d(c, word);
    data += 8;
    n -= 8;
  }
  while (n > 0) {
    c = __crc32b(c, static_cast<uint8_t>(*data));
    data++;
    n--;
  }
  return ~c;
#else
  return ExtendPortable(crc, data, n);
#endif
}

}  // namespace lsm::crc32
