#include "sstable.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "test_util.h"

namespace lsm {
namespace {

using GR = SSTableReader::GetResult;

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%010d", i);
  return buf;
}

std::string Value(int i) { return "value-" + std::to_string(i) + std::string(80, 'v'); }

// Writes keys 0, 2, 4, ... (even numbers only) so odd keys are misses that
// fall inside the table's key range.
void BuildEvenTable(const std::string& path, int n, size_t block_size = 4096) {
  std::unique_ptr<SSTableWriter> w;
  SSTableOptions opts;
  opts.block_size = block_size;
  ASSERT_TRUE(SSTableWriter::Open(path, opts, &w).ok());
  for (int i = 0; i < n; i++) {
    ASSERT_TRUE(w->Add(Key(2 * i), static_cast<SequenceNumber>(i + 1), ValueType::kValue,
                       Value(2 * i))
                    .ok());
  }
  ASSERT_TRUE(w->Finish(SyncMode::kNone).ok());
}

std::unique_ptr<SSTableReader> OpenTable(const std::string& path, bool use_bloom = true) {
  std::unique_ptr<SSTableReader> r;
  SSTableReadOptions ro;
  ro.use_bloom = use_bloom;
  Status s = SSTableReader::Open(path, &r, ro);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return r;
}

TEST(SSTableTest, PointLookupsHitAndMiss) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  BuildEvenTable(path, 5000);
  auto t = OpenTable(path);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->num_entries(), 5000u);
  EXPECT_GT(t->num_blocks(), 100u);
  EXPECT_EQ(t->smallest_key(), Key(0));
  EXPECT_EQ(t->largest_key(), Key(9998));
  EXPECT_EQ(t->min_seq(), 1u);
  EXPECT_EQ(t->max_seq(), 5000u);

  std::string v;
  GR r;
  for (int i = 0; i < 10000; i++) {
    ASSERT_TRUE(t->Get(Key(i), &v, &r).ok());
    if (i % 2 == 0) {
      ASSERT_EQ(r, GR::kFound) << i;
      EXPECT_EQ(v, Value(i));
    } else {
      ASSERT_EQ(r, GR::kNotFound) << i;
    }
  }
}

// The sparse index means each lookup reads exactly one data block, and keys
// outside [smallest, largest] read none. (Bloom filter off here, so misses
// inside the range really go to the block.)
TEST(SSTableTest, EachLookupReadsAtMostOneBlock) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  BuildEvenTable(path, 5000);
  auto t = OpenTable(path, /*use_bloom=*/false);
  std::string v;
  GR r;
  for (int i : {0, 1, 2, 777, 4242, 9997, 9998}) {
    uint64_t before = t->block_reads();
    ASSERT_TRUE(t->Get(Key(i), &v, &r).ok());
    EXPECT_EQ(t->block_reads() - before, 1u) << i;
  }
  for (const std::string& k : {std::string("a"), Key(9999), std::string("zzz")}) {
    uint64_t before = t->block_reads();
    ASSERT_TRUE(t->Get(k, &v, &r).ok());
    EXPECT_EQ(r, GR::kNotFound);
    EXPECT_EQ(t->block_reads() - before, 0u) << k;
  }
}

TEST(SSTableTest, BlocksAreAboutBlockSize) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  BuildEvenTable(path, 5000);
  auto t = OpenTable(path);
  // Each entry is 4 + 13 + 9 + 4 + ~96 = ~126 bytes; 5000 of them in ~4 KB
  // blocks gives about 5000 * 126 / 4096 = ~154 blocks.
  EXPECT_GE(t->num_blocks(), 140u);
  EXPECT_LE(t->num_blocks(), 160u);
}

TEST(SSTableTest, IteratorScansEverythingInOrder) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  BuildEvenTable(path, 3000);
  auto t = OpenTable(path);
  auto it = t->NewIterator();
  it->SeekToFirst();
  int i = 0;
  for (; it->Valid(); it->Next(), i++) {
    ASSERT_EQ(it->key(), Key(2 * i));
    ASSERT_EQ(it->value(), Value(2 * i));
    ASSERT_EQ(it->seq(), static_cast<SequenceNumber>(i + 1));
  }
  EXPECT_TRUE(it->status().ok());
  EXPECT_EQ(i, 3000);
}

TEST(SSTableTest, IteratorSeek) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  BuildEvenTable(path, 3000);
  auto t = OpenTable(path);
  auto it = t->NewIterator();
  it->Seek(Key(100));  // exact
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(100));
  it->Seek(Key(101));  // between keys
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(102));
  it->Seek("");  // before everything
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(0));
  it->Seek(Key(5999));  // after everything
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
  // Seek to every block boundary region and walk across it.
  for (int k = 1; k < 6000; k += 97) {
    it->Seek(Key(k));
    int expect = (k % 2 == 0) ? k : k + 1;
    for (int step = 0; step < 40 && expect < 6000; step++, expect += 2) {
      ASSERT_TRUE(it->Valid()) << k;
      ASSERT_EQ(it->key(), Key(expect));
      it->Next();
    }
  }
}

TEST(SSTableTest, TombstonesAndMultipleVersions) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  {
    std::unique_ptr<SSTableWriter> w;
    // Tiny blocks so versions of one key are spread over several blocks.
    SSTableOptions opts;
    opts.block_size = 64;
    ASSERT_TRUE(SSTableWriter::Open(path, opts, &w).ok());
    ASSERT_TRUE(w->Add("a", 5, ValueType::kValue, "a5").ok());
    for (SequenceNumber s = 40; s >= 10; s--) {  // "b": newest first
      ASSERT_TRUE(w->Add("b", s, ValueType::kValue, "b" + std::to_string(s)).ok());
    }
    ASSERT_TRUE(w->Add("c", 9, ValueType::kDeletion, "").ok());
    ASSERT_TRUE(w->Add("c", 3, ValueType::kValue, "c3").ok());
    ASSERT_TRUE(w->Finish(SyncMode::kNone).ok());
  }
  auto t = OpenTable(path);
  EXPECT_GT(t->num_blocks(), 5u);
  std::string v;
  GR r;
  ASSERT_TRUE(t->Get("b", &v, &r).ok());
  ASSERT_EQ(r, GR::kFound);
  EXPECT_EQ(v, "b40");
  ASSERT_TRUE(t->Get("c", &v, &r).ok());
  EXPECT_EQ(r, GR::kDeleted);
  ASSERT_TRUE(t->Get("a", &v, &r).ok());
  EXPECT_EQ(v, "a5");
  auto it = t->NewIterator();
  it->Seek("c");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->type(), ValueType::kDeletion);
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->seq(), 3u);
}

TEST(SSTableTest, ValueBiggerThanBlock) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  std::string big(20000, 'B');
  {
    std::unique_ptr<SSTableWriter> w;
    ASSERT_TRUE(SSTableWriter::Open(path, SSTableOptions{}, &w).ok());
    ASSERT_TRUE(w->Add("a", 1, ValueType::kValue, "small").ok());
    ASSERT_TRUE(w->Add("b", 2, ValueType::kValue, big).ok());
    ASSERT_TRUE(w->Add("c", 3, ValueType::kValue, "small").ok());
    ASSERT_TRUE(w->Finish(SyncMode::kFsync).ok());
  }
  auto t = OpenTable(path);
  std::string v;
  GR r;
  ASSERT_TRUE(t->Get("b", &v, &r).ok());
  EXPECT_EQ(v, big);
  ASSERT_TRUE(t->Get("c", &v, &r).ok());
  EXPECT_EQ(v, "small");
}

TEST(SSTableTest, WriterRejectsOutOfOrderAndEmpty) {
  test::TempDir dir;
  std::unique_ptr<SSTableWriter> w;
  ASSERT_TRUE(SSTableWriter::Open(dir.File("t.sst"), SSTableOptions{}, &w).ok());
  ASSERT_TRUE(w->Add("b", 1, ValueType::kValue, "").ok());
  EXPECT_FALSE(w->Add("a", 2, ValueType::kValue, "").ok());

  std::unique_ptr<SSTableWriter> w2;
  ASSERT_TRUE(SSTableWriter::Open(dir.File("t2.sst"), SSTableOptions{}, &w2).ok());
  ASSERT_TRUE(w2->Add("b", 1, ValueType::kValue, "").ok());
  EXPECT_FALSE(w2->Add("b", 1, ValueType::kValue, "").ok());  // duplicate (key, seq)
  EXPECT_FALSE(w2->Add("b", 2, ValueType::kValue, "").ok());  // same key, older must come after

  std::unique_ptr<SSTableWriter> w3;
  ASSERT_TRUE(SSTableWriter::Open(dir.File("t3.sst"), SSTableOptions{}, &w3).ok());
  EXPECT_FALSE(w3->Finish(SyncMode::kNone).ok());
}

void FlipByte(const std::string& path, uint64_t offset) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  f.seekg(static_cast<std::streamoff>(offset));
  char c;
  f.get(c);
  f.seekp(static_cast<std::streamoff>(offset));
  f.put(static_cast<char>(c ^ 0x5a));
}

TEST(SSTableTest, CorruptDataBlockIsReported) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  BuildEvenTable(path, 2000);
  FlipByte(path, 100);  // inside the first data block
  auto t = OpenTable(path);  // index and footer are fine
  std::string v;
  GR r;
  EXPECT_TRUE(t->Get(Key(0), &v, &r).IsCorruption());
  EXPECT_TRUE(t->Get(Key(2000), &v, &r).ok());  // other blocks still readable
  auto it = t->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(SSTableTest, CorruptFooterOrIndexFailsOpen) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  BuildEvenTable(path, 2000);
  uint64_t size = std::filesystem::file_size(path);

  std::string bad_magic = dir.File("magic.sst");
  std::filesystem::copy_file(path, bad_magic);
  FlipByte(bad_magic, size - 1);
  std::unique_ptr<SSTableReader> r;
  EXPECT_TRUE(SSTableReader::Open(bad_magic, &r).IsCorruption());

  std::string bad_index = dir.File("index.sst");
  std::filesystem::copy_file(path, bad_index);
  FlipByte(bad_index, size - 56 - 200);  // in the meta blocks just before the footer
  EXPECT_TRUE(SSTableReader::Open(bad_index, &r).IsCorruption());

  std::string truncated = dir.File("short.sst");
  std::filesystem::copy_file(path, truncated);
  std::filesystem::resize_file(truncated, size / 2);
  EXPECT_FALSE(SSTableReader::Open(truncated, &r).ok());
}

// Random keys with random versions and tombstones, checked against a model.
TEST(SSTableTest, RandomTableMatchesModel) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  std::mt19937_64 rng(77);
  // (key, seq desc) -> (type, value)
  std::map<std::tuple<std::string, SequenceNumber>, std::pair<ValueType, std::string>,
           bool (*)(const std::tuple<std::string, SequenceNumber>&,
                    const std::tuple<std::string, SequenceNumber>&)>
      entries([](const auto& a, const auto& b) {
        return CompareInternal(std::get<0>(a), std::get<1>(a), std::get<0>(b), std::get<1>(b)) < 0;
      });
  for (int i = 0; i < 20000; i++) {
    std::string k = "k" + std::to_string(rng() % 3000);
    SequenceNumber s = rng() % 1000000 + 1;
    bool del = rng() % 5 == 0;
    entries[{k, s}] = {del ? ValueType::kDeletion : ValueType::kValue,
                       del ? "" : std::string(rng() % 200, 'a' + static_cast<char>(rng() % 26))};
  }
  {
    std::unique_ptr<SSTableWriter> w;
    ASSERT_TRUE(SSTableWriter::Open(path, SSTableOptions{}, &w).ok());
    for (const auto& [ks, tv] : entries) {
      ASSERT_TRUE(w->Add(std::get<0>(ks), std::get<1>(ks), tv.first, tv.second).ok());
    }
    ASSERT_TRUE(w->Finish(SyncMode::kNone).ok());
  }
  auto t = OpenTable(path);
  // Newest version per key from the model.
  std::map<std::string, std::pair<ValueType, std::string>> newest;
  for (const auto& [ks, tv] : entries) newest.emplace(std::get<0>(ks), tv);  // first = newest
  for (int i = 0; i < 3100; i++) {
    std::string k = "k" + std::to_string(i);
    std::string v;
    GR r;
    ASSERT_TRUE(t->Get(k, &v, &r).ok());
    auto it = newest.find(k);
    if (it == newest.end()) {
      EXPECT_EQ(r, GR::kNotFound) << k;
    } else if (it->second.first == ValueType::kDeletion) {
      EXPECT_EQ(r, GR::kDeleted) << k;
    } else {
      ASSERT_EQ(r, GR::kFound) << k;
      EXPECT_EQ(v, it->second.second);
    }
  }
  // Full scan returns every entry, every version, in order.
  auto it = t->NewIterator();
  it->SeekToFirst();
  for (const auto& [ks, tv] : entries) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key(), std::get<0>(ks));
    ASSERT_EQ(it->seq(), std::get<1>(ks));
    ASSERT_EQ(it->type(), tv.first);
    ASSERT_EQ(it->value(), tv.second);
    it->Next();
  }
  EXPECT_FALSE(it->Valid());
}

// Gets from several threads at once share one reader (run under TSan).
TEST(SSTableTest, ConcurrentGets) {
  test::TempDir dir;
  std::string path = dir.File("t.sst");
  BuildEvenTable(path, 5000);
  auto t = OpenTable(path, /*use_bloom=*/false);
  std::vector<std::thread> threads;
  std::atomic<int> errors{0};
  std::atomic<uint64_t> in_range{0};  // lookups inside [smallest, largest] read one block
  for (int th = 0; th < 4; th++) {
    threads.emplace_back([&, th] {
      std::mt19937 rng(th);
      std::string v;
      GR r;
      for (int i = 0; i < 2000; i++) {
        int k = static_cast<int>(rng() % 10000);
        if (k <= 9998) in_range++;
        if (!t->Get(Key(k), &v, &r).ok()) errors++;
        if ((k % 2 == 0) != (r == GR::kFound)) errors++;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(t->block_reads(), in_range.load());
}

}  // namespace
}  // namespace lsm
