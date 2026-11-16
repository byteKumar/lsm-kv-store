#pragma once

// Bloom filter: a bit array that answers "is this key possibly in the set?"
// "No" is always right (no false negatives). "Maybe" is wrong with a small,
// tunable probability (the false-positive rate).
//
// Building: for each key, set k bits chosen by k hash functions.
// Checking: if any of the key's k bits is 0, the key was never added.
//
// With m bits, n keys and k hash functions, the expected false-positive rate
// is (1 - e^(-k*n/m))^k, smallest at k = (m/n) * ln 2. At 10 bits per key
// that is k = 7 and about 0.82%.
//
// The k hash functions come from one 64-bit hash split into two 32-bit
// halves h1, h2: probe i uses bit (h1 + i*h2) mod m ("double hashing",
// Kirsch and Mitzenmacher 2006), which keeps the same false-positive rate as
// k independent hashes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

// MurmurHash64A (Austin Appleby), a fast non-cryptographic 64-bit hash.
uint64_t Hash64(const char* data, size_t n, uint64_t seed = 0x9747b28c);

class BloomFilterBuilder {
 public:
  explicit BloomFilterBuilder(int bits_per_key);
  void AddKey(std::string_view key);
  size_t NumKeys() const { return hashes_.size(); }
  // Encoded filter: [bit array][num_probes u8].
  std::string Finish() const;

  static int NumProbes(int bits_per_key);

 private:
  int bits_per_key_;
  std::vector<uint64_t> hashes_;  // one per key, so the bit array can be sized at the end
};

// Checks a key against an encoded filter. An empty filter means "no filter":
// everything may match.
bool BloomKeyMayMatch(std::string_view filter, std::string_view key);

// Expected false-positive rate for n keys in m bits with k probes.
double BloomTheoreticalFpRate(uint64_t num_keys, uint64_t num_bits, int num_probes);

}  // namespace lsm
