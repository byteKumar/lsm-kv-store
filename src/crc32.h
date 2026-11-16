#pragma once

// CRC-32 (the IEEE/zlib polynomial). Detects torn or corrupted records; it is
// not a cryptographic hash. Uses the ARMv8 CRC32 instructions when the CPU
// has them (all Apple Silicon does) and a lookup table otherwise.

#include <cstddef>
#include <cstdint>

namespace lsm::crc32 {

// Continues a CRC over more data: Extend(Value(a), b) == Value(a + b).
uint32_t Extend(uint32_t crc, const char* data, size_t n);
inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// Table-based version, kept so tests can check the hardware path against it.
uint32_t ExtendPortable(uint32_t crc, const char* data, size_t n);

}  // namespace lsm::crc32
