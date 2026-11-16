#include "bloom.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "sstable.h"
#include "test_util.h"

namespace lsm {
namespace {

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%010d", i);
  return buf;
}

TEST(BloomTest, ProbeCountFollowsLn2) {
  EXPECT_EQ(BloomFilterBuilder::NumProbes(10), 7);  // 10 * 0.693 = 6.93
  EXPECT_EQ(BloomFilterBuilder::NumProbes(1), 1);
  EXPECT_EQ(BloomFilterBuilder::NumProbes(100), 30);  // capped
}

TEST(BloomTest, EmptyFilterMatchesEverything) {
  EXPECT_TRUE(BloomKeyMayMatch("", "anything"));
}

TEST(BloomTest, HashIsDeterministicAndSeedDependent) {
  std::string s = "hello world, this is a key";
  for (size_t n = 0; n <= s.size(); n++) {
    EXPECT_EQ(Hash64(s.data(), n), Hash64(s.data(), n));
    EXPECT_NE(Hash64(s.data(), n, 1), Hash64(s.data(), n, 2));
  }
}

TEST(BloomTest, NoFalseNegatives) {
  for (int bits : {1, 4, 10, 20}) {
    BloomFilterBuilder b(bits);
    for (int i = 0; i < 20000; i++) b.AddKey(Key(i));
    std::string f = b.Finish();
    for (int i = 0; i < 20000; i++) ASSERT_TRUE(BloomKeyMayMatch(f, Key(i))) << bits << " " << i;
  }
}

TEST(BloomTest, SmallFilterStillWorks) {
  BloomFilterBuilder b(10);
  b.AddKey("only");
  std::string f = b.Finish();
  EXPECT_EQ(f.size(), 64u / 8 + 1);  // minimum 64 bits + probe byte
  EXPECT_TRUE(BloomKeyMayMatch(f, "only"));
}

// Measured false-positive rate should sit close to (1 - e^(-kn/m))^k. With
// 200,000 absent keys the sampling error is about +/- 0.02 percentage points
// at 10 bits/key, so a 25% window is generous but still catches a bad hash.
TEST(BloomTest, FalsePositiveRateMatchesTheory) {
  constexpr int kKeys = 100000;
  constexpr int kProbesTried = 200000;
  for (int bits : {6, 10, 14}) {
    BloomFilterBuilder b(bits);
    for (int i = 0; i < kKeys; i++) b.AddKey(Key(2 * i));  // even keys in the set
    std::string f = b.Finish();
    int fp = 0;
    for (int i = 0; i < kProbesTried; i++) {
      if (BloomKeyMayMatch(f, Key(2 * i + 1))) fp++;  // odd keys never added
    }
    double measured = static_cast<double>(fp) / kProbesTried;
    double theory = BloomTheoreticalFpRate(kKeys, (f.size() - 1) * 8,
                                           BloomFilterBuilder::NumProbes(bits));
    std::printf("  bits/key=%d  measured FP=%.4f%%  theory=%.4f%%\n", bits, 100 * measured,
                100 * theory);
    EXPECT_GT(measured, theory * 0.75) << bits;
    EXPECT_LT(measured, theory * 1.25) << bits;
  }
}

class SSTableBloomTest : public ::testing::Test {
 protected:
  void Build(const std::string& path, int bits) {
    std::unique_ptr<SSTableWriter> w;
    SSTableOptions opts;
    opts.bloom_bits_per_key = bits;
    ASSERT_TRUE(SSTableWriter::Open(path, opts, &w).ok());
    for (int i = 0; i < kKeys; i++) {
      ASSERT_TRUE(w->Add(Key(2 * i), static_cast<SequenceNumber>(i + 1), ValueType::kValue,
                         std::string(100, 'v'))
                      .ok());
    }
    ASSERT_TRUE(w->Finish(SyncMode::kNone).ok());
  }
  // Looks up every odd key (all inside the key range, none present).
  void LookupMisses(const SSTableReader& t) {
    std::string v;
    SSTableReader::GetResult r;
    for (int i = 0; i < kKeys - 1; i++) {
      ASSERT_TRUE(t.Get(Key(2 * i + 1), &v, &r).ok());
      ASSERT_EQ(r, SSTableReader::GetResult::kNotFound);
    }
  }
  static constexpr int kKeys = 20000;
  test::TempDir dir_;
};

TEST_F(SSTableBloomTest, FilterSkipsMostBlockReadsForMisses) {
  std::string path = dir_.File("t.sst");
  Build(path, 10);
  std::unique_ptr<SSTableReader> t;
  ASSERT_TRUE(SSTableReader::Open(path, &t).ok());
  EXPECT_EQ(t->num_keys(), static_cast<uint64_t>(kKeys));
  EXPECT_EQ(t->filter_probes(), 7);
  EXPECT_EQ(t->filter_bits(), static_cast<uint64_t>(kKeys) * 10);
  LookupMisses(*t);
  const uint64_t misses = kKeys - 1;
  EXPECT_EQ(t->bloom_checks(), misses);
  // Every block read for a miss is a false positive, and nothing else reads.
  EXPECT_EQ(t->block_reads(), t->bloom_false_positives());
  EXPECT_EQ(t->bloom_negatives() + t->bloom_false_positives(), misses);
  EXPECT_LT(t->block_reads(), misses / 50);  // ~0.8% expected; far below 2%

  // Hits still work and are not counted as false positives.
  std::string v;
  SSTableReader::GetResult r;
  uint64_t fp_before = t->bloom_false_positives();
  ASSERT_TRUE(t->Get(Key(42), &v, &r).ok());
  EXPECT_EQ(r, SSTableReader::GetResult::kFound);
  EXPECT_EQ(t->bloom_false_positives(), fp_before);
}

TEST_F(SSTableBloomTest, IgnoringFilterReadsOneBlockPerMiss) {
  std::string path = dir_.File("t.sst");
  Build(path, 10);
  std::unique_ptr<SSTableReader> t;
  SSTableReadOptions ro;
  ro.use_bloom = false;
  ASSERT_TRUE(SSTableReader::Open(path, &t, ro).ok());
  LookupMisses(*t);
  EXPECT_EQ(t->block_reads(), static_cast<uint64_t>(kKeys - 1));
  EXPECT_EQ(t->bloom_checks(), 0u);
}

TEST_F(SSTableBloomTest, TableWithoutFilter) {
  std::string path = dir_.File("t.sst");
  Build(path, 0);
  std::unique_ptr<SSTableReader> t;
  ASSERT_TRUE(SSTableReader::Open(path, &t).ok());
  EXPECT_EQ(t->filter_bits(), 0u);
  LookupMisses(*t);
  EXPECT_EQ(t->block_reads(), static_cast<uint64_t>(kKeys - 1));
}

TEST(SSTableBloomVersions, EachKeyAddedOnce) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  std::unique_ptr<SSTableWriter> w;
  ASSERT_TRUE(SSTableWriter::Open(path, SSTableOptions{}, &w).ok());
  ASSERT_TRUE(w->Add("a", 3, ValueType::kValue, "x").ok());
  ASSERT_TRUE(w->Add("a", 2, ValueType::kDeletion, "").ok());
  ASSERT_TRUE(w->Add("a", 1, ValueType::kValue, "y").ok());
  ASSERT_TRUE(w->Add("b", 4, ValueType::kDeletion, "").ok());
  ASSERT_TRUE(w->Finish(SyncMode::kNone).ok());
  std::unique_ptr<SSTableReader> t;
  ASSERT_TRUE(SSTableReader::Open(path, &t).ok());
  EXPECT_EQ(t->num_entries(), 4u);
  EXPECT_EQ(t->num_keys(), 2u);
  std::string v;
  SSTableReader::GetResult r;
  ASSERT_TRUE(t->Get("b", &v, &r).ok());
  EXPECT_EQ(r, SSTableReader::GetResult::kDeleted);  // tombstones are in the filter too
  EXPECT_EQ(t->bloom_false_positives(), 0u);
}

}  // namespace
}  // namespace lsm
