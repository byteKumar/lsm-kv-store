#pragma once

// Fixed-width little-endian integer encoding for on-disk formats. Written
// byte by byte so the file format does not depend on the CPU's byte order.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lsm {

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  for (int i = 0; i < 4; i++) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst->append(buf, 4);
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; i++) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst->append(buf, 8);
}

inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

// Length-prefixed string: fixed32 length, then the bytes.
inline void PutLengthPrefixed(std::string* dst, std::string_view s) {
  PutFixed32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s.data(), s.size());
}

// Reads a length-prefixed string from the front of *input and advances it.
// Returns false if *input is too short.
inline bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  if (input->size() < 4) return false;
  uint32_t len = DecodeFixed32(input->data());
  if (input->size() - 4 < len) return false;
  *result = input->substr(4, len);
  input->remove_prefix(4 + len);
  return true;
}

}  // namespace lsm
