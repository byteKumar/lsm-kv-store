#include "memtable.h"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <random>
#include <string>

namespace lsm {
namespace {

using GR = MemTable::GetResult;

TEST(MemTableTest, PutAndGet) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "apple", "red");
  mem.Add(2, ValueType::kValue, "banana", "yellow");
  std::string v;
  EXPECT_EQ(mem.Get("apple", &v), GR::kFound);
  EXPECT_EQ(v, "red");
  EXPECT_EQ(mem.Get("banana", &v), GR::kFound);
  EXPECT_EQ(v, "yellow");
  EXPECT_EQ(mem.Get("cherry", &v), GR::kNotFound);
  EXPECT_EQ(mem.Get("", &v), GR::kNotFound);
}

TEST(MemTableTest, NewestVersionWins) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "k", "v1");
  mem.Add(2, ValueType::kValue, "k", "v2");
  mem.Add(3, ValueType::kValue, "k", "v3");
  std::string v;
  ASSERT_EQ(mem.Get("k", &v), GR::kFound);
  EXPECT_EQ(v, "v3");
  EXPECT_EQ(mem.NumEntries(), 3u);  // old versions are kept, not overwritten
}

TEST(MemTableTest, TombstoneHidesValueAndCanBeOverwritten) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "k", "v1");
  mem.Add(2, ValueType::kDeletion, "k", "");
  std::string v;
  EXPECT_EQ(mem.Get("k", &v), GR::kDeleted);
  mem.Add(3, ValueType::kValue, "k", "v3");
  ASSERT_EQ(mem.Get("k", &v), GR::kFound);
  EXPECT_EQ(v, "v3");
}

TEST(MemTableTest, KeyPrefixesAreDistinct) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "ab", "1");
  mem.Add(2, ValueType::kValue, "abc", "2");
  std::string v;
  EXPECT_EQ(mem.Get("a", &v), GR::kNotFound);
  ASSERT_EQ(mem.Get("ab", &v), GR::kFound);
  EXPECT_EQ(v, "1");
  ASSERT_EQ(mem.Get("abc", &v), GR::kFound);
  EXPECT_EQ(v, "2");
}

TEST(MemTableTest, BinaryKeysAndValues) {
  MemTable mem;
  std::string k("a\0b", 3), val("\0\xff\0", 3);
  mem.Add(1, ValueType::kValue, k, val);
  std::string v;
  EXPECT_EQ(mem.Get(std::string("a", 1), &v), GR::kNotFound);
  ASSERT_EQ(mem.Get(k, &v), GR::kFound);
  EXPECT_EQ(v, val);
}

TEST(MemTableTest, IteratorOrderIsKeyAscSeqDesc) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "b", "b1");
  mem.Add(2, ValueType::kValue, "a", "a2");
  mem.Add(3, ValueType::kDeletion, "b", "");
  mem.Add(4, ValueType::kValue, "c", "c4");
  MemTable::Iterator it(&mem);
  it.SeekToFirst();
  std::vector<std::pair<std::string, SequenceNumber>> got;
  for (; it.Valid(); it.Next()) got.emplace_back(it.entry().key, it.entry().seq);
  std::vector<std::pair<std::string, SequenceNumber>> want = {{"a", 2}, {"b", 3}, {"b", 1}, {"c", 4}};
  EXPECT_EQ(got, want);

  it.Seek("b");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.entry().key, "b");
  EXPECT_EQ(it.entry().seq, 3u);
  EXPECT_EQ(it.entry().type, ValueType::kDeletion);
}

TEST(MemTableTest, MemoryUsageGrows) {
  MemTable mem;
  EXPECT_EQ(mem.ApproximateMemoryUsage(), 0u);
  mem.Add(1, ValueType::kValue, std::string(16, 'k'), std::string(100, 'v'));
  size_t one = mem.ApproximateMemoryUsage();
  EXPECT_GE(one, 116u);
  mem.Add(2, ValueType::kValue, std::string(16, 'j'), std::string(100, 'v'));
  EXPECT_EQ(mem.ApproximateMemoryUsage(), 2 * one);
}

// Random puts and deletes checked against std::map as the reference model.
TEST(MemTableTest, RandomOpsMatchModel) {
  MemTable mem;
  std::map<std::string, std::optional<std::string>> model;  // nullopt = deleted
  std::mt19937_64 rng(1234);
  SequenceNumber seq = 0;
  for (int i = 0; i < 20000; i++) {
    std::string key = "key" + std::to_string(rng() % 2000);
    if (rng() % 4 == 0) {
      mem.Add(++seq, ValueType::kDeletion, key, "");
      model[key] = std::nullopt;
    } else {
      std::string val = "val" + std::to_string(rng());
      mem.Add(++seq, ValueType::kValue, key, val);
      model[key] = val;
    }
  }
  for (int k = 0; k < 2100; k++) {
    std::string key = "key" + std::to_string(k);
    std::string v;
    GR r = mem.Get(key, &v);
    auto it = model.find(key);
    if (it == model.end()) {
      EXPECT_EQ(r, GR::kNotFound) << key;
    } else if (!it->second) {
      EXPECT_EQ(r, GR::kDeleted) << key;
    } else {
      ASSERT_EQ(r, GR::kFound) << key;
      EXPECT_EQ(v, *it->second);
    }
  }
}

}  // namespace
}  // namespace lsm
