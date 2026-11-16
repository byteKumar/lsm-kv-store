#include "bloom.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lsm {

uint64_t Hash64(const char* data, size_t n, uint64_t seed) {
  constexpr uint64_t m = 0xc6a4a7935bd1e995ULL;
  constexpr int r = 47;
  uint64_t h = seed ^ (n * m);
  const char* p = data;
  const char* end = data + (n / 8) * 8;
  for (; p != end; p += 8) {
    uint64_t k;
    std::memcpy(&k, p, 8);
    k *= m;
    k ^= k >> r;
    k *= m;
    h ^= k;
    h *= m;
  }
  const auto* tail = reinterpret_cast<const uint8_t*>(p);
  switch (n & 7) {
    case 7: h ^= static_cast<uint64_t>(tail[6]) << 48; [[fallthrough]];
    case 6: h ^= static_cast<uint64_t>(tail[5]) << 40; [[fallthrough]];
    case 5: h ^= static_cast<uint64_t>(tail[4]) << 32; [[fallthrough]];
    case 4: h ^= static_cast<uint64_t>(tail[3]) << 24; [[fallthrough]];
    case 3: h ^= static_cast<uint64_t>(tail[2]) << 16; [[fallthrough]];
    case 2: h ^= static_cast<uint64_t>(tail[1]) << 8; [[fallthrough]];
    case 1:
      h ^= static_cast<uint64_t>(tail[0]);
      h *= m;
  }
  h ^= h >> r;
  h *= m;
  h ^= h >> r;
  return h;
}

int BloomFilterBuilder::NumProbes(int bits_per_key) {
  int k = static_cast<int>(std::lround(bits_per_key * 0.69314718056));  // ln 2
  return std::clamp(k, 1, 30);
}

BloomFilterBuilder::BloomFilterBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {}

void BloomFilterBuilder::AddKey(std::string_view key) {
  hashes_.push_back(Hash64(key.data(), key.size()));
}

namespace {
inline uint64_t ProbeBit(uint64_t h, int i, uint64_t num_bits) {
  uint64_t h1 = h & 0xffffffffu;
  uint64_t h2 = h >> 32;
  return (h1 + static_cast<uint64_t>(i) * h2) % num_bits;
}
}  // namespace

std::string BloomFilterBuilder::Finish() const {
  // Round up to whole bytes; at least 64 bits so tiny tables still work.
  uint64_t bits = std::max<uint64_t>(64, hashes_.size() * static_cast<uint64_t>(bits_per_key_));
  uint64_t bytes = (bits + 7) / 8;
  bits = bytes * 8;
  int k = NumProbes(bits_per_key_);
  std::string out(bytes, '\0');
  for (uint64_t h : hashes_) {
    for (int i = 0; i < k; i++) {
      uint64_t b = ProbeBit(h, i, bits);
      out[b / 8] = static_cast<char>(out[b / 8] | (1 << (b % 8)));
    }
  }
  out.push_back(static_cast<char>(k));
  return out;
}

bool BloomKeyMayMatch(std::string_view filter, std::string_view key) {
  if (filter.size() < 2) return true;  // no filter (or nothing usable): can't rule anything out
  int k = static_cast<uint8_t>(filter.back());
  if (k < 1 || k > 30) return true;  // unknown encoding: be safe, never a false negative
  uint64_t bits = (filter.size() - 1) * 8;
  uint64_t h = Hash64(key.data(), key.size());
  for (int i = 0; i < k; i++) {
    uint64_t b = ProbeBit(h, i, bits);
    if ((static_cast<uint8_t>(filter[b / 8]) & (1 << (b % 8))) == 0) return false;
  }
  return true;
}

double BloomTheoreticalFpRate(uint64_t num_keys, uint64_t num_bits, int num_probes) {
  double k = num_probes;
  return std::pow(1.0 - std::exp(-k * static_cast<double>(num_keys) / static_cast<double>(num_bits)), k);
}

}  // namespace lsm
