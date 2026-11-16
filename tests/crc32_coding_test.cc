#include <gtest/gtest.h>

#include <random>
#include <string>

#include "coding.h"
#include "crc32.h"

namespace lsm {
namespace {

TEST(Crc32Test, KnownVectors) {
  // Standard check value for CRC-32/ISO-HDLC (the zlib/PNG CRC).
  EXPECT_EQ(crc32::Value("123456789", 9), 0xCBF43926u);
  EXPECT_EQ(crc32::ExtendPortable(0, "123456789", 9), 0xCBF43926u);
  EXPECT_EQ(crc32::Value("", 0), 0u);
}

TEST(Crc32Test, HardwareMatchesTableForAllLengthsAndAlignments) {
  std::mt19937 rng(9);
  std::string buf(2048, '\0');
  for (char& c : buf) c = static_cast<char>(rng());
  for (size_t off = 0; off < 8; off++) {
    for (size_t len = 0; len < 600; len++) {
      ASSERT_EQ(crc32::Value(buf.data() + off, len),
                crc32::ExtendPortable(0, buf.data() + off, len))
          << "off=" << off << " len=" << len;
    }
  }
}

TEST(Crc32Test, ExtendComposes) {
  std::string a = "hello, ", b = "world";
  std::string ab = a + b;
  EXPECT_EQ(crc32::Extend(crc32::Value(a.data(), a.size()), b.data(), b.size()),
            crc32::Value(ab.data(), ab.size()));
}

TEST(Crc32Test, DetectsSingleBitFlips) {
  std::string s = "some record payload";
  uint32_t base = crc32::Value(s.data(), s.size());
  for (size_t i = 0; i < s.size() * 8; i++) {
    std::string t = s;
    t[i / 8] ^= static_cast<char>(1 << (i % 8));
    EXPECT_NE(crc32::Value(t.data(), t.size()), base) << "bit " << i;
  }
}

TEST(CodingTest, FixedRoundTrip) {
  std::string s;
  PutFixed32(&s, 0xdeadbeef);
  PutFixed64(&s, 0x0123456789abcdefULL);
  ASSERT_EQ(s.size(), 12u);
  EXPECT_EQ(static_cast<uint8_t>(s[0]), 0xef);  // little-endian on disk
  EXPECT_EQ(DecodeFixed32(s.data()), 0xdeadbeefu);
  EXPECT_EQ(DecodeFixed64(s.data() + 4), 0x0123456789abcdefULL);
}

TEST(CodingTest, LengthPrefixed) {
  std::string s;
  PutLengthPrefixed(&s, "abc");
  PutLengthPrefixed(&s, "");
  std::string_view in(s), out;
  ASSERT_TRUE(GetLengthPrefixed(&in, &out));
  EXPECT_EQ(out, "abc");
  ASSERT_TRUE(GetLengthPrefixed(&in, &out));
  EXPECT_EQ(out, "");
  EXPECT_TRUE(in.empty());
  EXPECT_FALSE(GetLengthPrefixed(&in, &out));
  std::string bad;
  PutFixed32(&bad, 10);
  bad += "short";
  std::string_view bin(bad);
  EXPECT_FALSE(GetLengthPrefixed(&bin, &out));
}

}  // namespace
}  // namespace lsm
