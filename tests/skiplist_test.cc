#include "skiplist.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace lsm {
namespace {

struct IntCompare {
  int operator()(uint64_t a, uint64_t b) const { return a < b ? -1 : (a > b ? 1 : 0); }
};
using IntList = SkipList<uint64_t, IntCompare>;

TEST(SkipListTest, Empty) {
  IntList list(IntCompare{});
  EXPECT_FALSE(list.Contains(10));
  IntList::Iterator it(&list);
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.Seek(100);
  EXPECT_FALSE(it.Valid());
}

TEST(SkipListTest, InsertAndLookupMatchesStdSet) {
  IntList list(IntCompare{});
  std::set<uint64_t> model;
  std::mt19937_64 rng(42);
  for (int i = 0; i < 5000; i++) {
    uint64_t k = rng() % 20000;
    if (model.insert(k).second) list.Insert(k);
  }
  for (uint64_t k = 0; k < 20000; k++) {
    ASSERT_EQ(list.Contains(k), model.count(k) == 1) << k;
  }
  // Full iteration returns keys in sorted order.
  IntList::Iterator it(&list);
  it.SeekToFirst();
  for (uint64_t expected : model) {
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.key(), expected);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());
}

TEST(SkipListTest, SeekFindsFirstGreaterOrEqual) {
  IntList list(IntCompare{});
  for (uint64_t k : {10, 20, 30, 40}) list.Insert(k);
  IntList::Iterator it(&list);
  it.Seek(20);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 20u);
  it.Seek(21);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 30u);
  it.Seek(0);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 10u);
  it.Seek(41);
  EXPECT_FALSE(it.Valid());
}

// One writer inserts while several readers search and iterate. Every key the
// writer has announced as inserted must be visible to readers, and iteration
// must always be sorted. Run under TSan to check the memory ordering.
TEST(SkipListTest, ConcurrentReadersWithOneWriter) {
  constexpr int kKeys = 20000;
  constexpr int kReaders = 4;
  IntList list(IntCompare{});
  std::vector<uint64_t> keys(kKeys);
  std::iota(keys.begin(), keys.end(), 0);
  std::shuffle(keys.begin(), keys.end(), std::mt19937_64(7));

  std::atomic<int> inserted{0};
  std::atomic<bool> failed{false};

  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; r++) {
    readers.emplace_back([&, r] {
      std::mt19937_64 rng(100 + r);
      while (inserted.load(std::memory_order_acquire) < kKeys) {
        int n = inserted.load(std::memory_order_acquire);
        if (n > 0 && !list.Contains(keys[rng() % n])) failed = true;
        // Iterate a short stretch and check ordering.
        IntList::Iterator it(&list);
        it.Seek(rng() % kKeys);
        uint64_t prev = 0;
        bool first = true;
        for (int i = 0; i < 50 && it.Valid(); i++, it.Next()) {
          if (!first && it.key() <= prev) failed = true;
          prev = it.key();
          first = false;
        }
      }
    });
  }
  for (int i = 0; i < kKeys; i++) {
    list.Insert(keys[i]);
    inserted.store(i + 1, std::memory_order_release);
  }
  for (auto& t : readers) t.join();
  EXPECT_FALSE(failed.load());
  for (uint64_t k = 0; k < kKeys; k++) ASSERT_TRUE(list.Contains(k));
}

}  // namespace
}  // namespace lsm
