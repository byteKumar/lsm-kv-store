#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>

#include "db.h"
#include "test_util.h"

namespace lsm {
namespace {

namespace fs = std::filesystem;

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%010d", i);
  return buf;
}

std::string GetOrMissing(DB* db, std::string_view key) {
  std::string v;
  Status s = db->Get(key, &v);
  if (s.IsNotFound()) return "<missing>";
  EXPECT_TRUE(s.ok()) << s.ToString();
  return v;
}

size_t CountSst(const std::string& dir) {
  size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) n += e.path().extension() == ".sst";
  return n;
}

bool Pick(const std::vector<uint64_t>& sizes, size_t* b, size_t* e) {
  Options o;  // min 4, max 16, ratio 2
  return PickSizeTieredRun(sizes, o, b, e);
}

TEST(PickSizeTieredRunTest, NeedsMinTables) {
  size_t b, e;
  EXPECT_FALSE(Pick({}, &b, &e));
  EXPECT_FALSE(Pick({10, 10, 10}, &b, &e));
  ASSERT_TRUE(Pick({10, 10, 10, 10}, &b, &e));
  EXPECT_EQ(b, 0u);
  EXPECT_EQ(e, 4u);
}

TEST(PickSizeTieredRunTest, PicksNewestSimilarRun) {
  size_t b, e;
  // A big old table, then four small new ones: merge just the small ones.
  ASSERT_TRUE(Pick({1000, 10, 12, 9, 11}, &b, &e));
  EXPECT_EQ(b, 1u);
  EXPECT_EQ(e, 5u);
  // Two separate tiers that both qualify: the newer one goes first.
  ASSERT_TRUE(Pick({100, 100, 100, 100, 10, 10, 10, 10}, &b, &e));
  EXPECT_EQ(b, 4u);
  EXPECT_EQ(e, 8u);
}

TEST(PickSizeTieredRunTest, RatioBreaksRuns) {
  size_t b, e;
  EXPECT_FALSE(Pick({10, 30, 10, 30, 10}, &b, &e));  // neighbours 3x apart
  // The newest table is an outlier; the run before it still qualifies.
  ASSERT_TRUE(Pick({10, 10, 15, 20, 500}, &b, &e));
  EXPECT_EQ(b, 0u);
  EXPECT_EQ(e, 4u);
}

// Regression test: sizes that each differ by about 2x never form a run. With
// the table-count stall on, that used to block writers forever (found by
// ConcurrentLoadWithCompaction hanging). At the stall limit a merge must be
// picked anyway.
TEST(PickSizeTieredRunTest, ForcedMergeAtStallLimit) {
  Options o;
  o.stall_table_count = 6;
  size_t b, e;
  std::vector<uint64_t> sizes = {3200, 1600, 800, 400, 200, 100};
  EXPECT_FALSE(PickSizeTieredRun({3200, 1600, 800, 400, 200}, o, &b, &e));  // under the limit
  ASSERT_TRUE(PickSizeTieredRun(sizes, o, &b, &e));
  EXPECT_EQ(b, 2u);  // the four smallest, newest tables
  EXPECT_EQ(e, 6u);
}

TEST(PickSizeTieredRunTest, CapsRunLength) {
  size_t b, e;
  std::vector<uint64_t> sizes(40, 7);
  ASSERT_TRUE(Pick(sizes, &b, &e));
  EXPECT_EQ(e, 40u);
  EXPECT_EQ(e - b, 16u);
}

Options SmallOptions() {
  Options o;
  o.memtable_size = 32 << 10;
  return o;
}

// Sequential load: tables stay few and data stays correct, across reopen.
TEST(CompactionTest, KeepsTableCountLowAndDataCorrect) {
  test::TempDir dir;
  std::map<std::string, std::optional<std::string>> model;
  std::mt19937_64 rng(11);
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(SmallOptions(), dir.path(), &db).ok());
    for (int i = 0; i < 60000; i++) {
      std::string k = Key(static_cast<int>(rng() % 10000));
      if (rng() % 6 == 0) {
        ASSERT_TRUE(db->Delete(k).ok());
        model[k] = std::nullopt;
      } else {
        std::string v = std::to_string(i) + std::string(rng() % 60, 'z');
        ASSERT_TRUE(db->Put(k, v).ok());
        model[k] = v;
      }
    }
    ASSERT_TRUE(db->WaitForIdle().ok());
    DBStats st = db->GetStats();
    EXPECT_GT(st.flushes, 100u);
    EXPECT_GT(st.compactions, 20u);
    EXPECT_GT(st.versions_dropped, 0u);
    // Size-tiered with 4-way merges keeps a few tables per tier.
    EXPECT_LT(st.num_tables, 20u) << "flushes=" << st.flushes;
    EXPECT_EQ(CountSst(dir.path()), st.num_tables);  // inputs were deleted
    EXPECT_GT(st.compaction_block_reads, 0u);
    // Compaction reads are counted apart: lookups below must not see them.
    uint64_t reads_before = st.block_reads;
    for (int i = 0; i < 10000; i++) {
      auto it = model.find(Key(i));
      std::string want = (it == model.end() || !it->second) ? "<missing>" : *it->second;
      ASSERT_EQ(GetOrMissing(db.get(), Key(i)), want) << i;
    }
    // At most one block per table per lookup.
    EXPECT_LE(db->GetStats().block_reads - reads_before, 10000u * st.num_tables);
  }
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(SmallOptions(), dir.path(), &db).ok());
  std::vector<std::pair<std::string, std::string>> got, want;
  for (const auto& [k, v] : model) {
    if (v) want.emplace_back(k, *v);
  }
  ASSERT_TRUE(db->Scan("", 1u << 30, &got).ok());
  EXPECT_EQ(got, want);
}

TEST(CompactionTest, CompactAllDropsTombstonesAndOldVersions) {
  test::TempDir dir;
  std::unique_ptr<DB> db;
  Options o = SmallOptions();
  o.compaction = false;  // only the manual compaction runs
  ASSERT_TRUE(DB::Open(o, dir.path(), &db).ok());
  for (int i = 0; i < 3000; i++) ASSERT_TRUE(db->Put(Key(i), std::string(50, 'a')).ok());
  for (int i = 0; i < 3000; i++) ASSERT_TRUE(db->Put(Key(i), std::string(50, 'b')).ok());
  for (int i = 0; i < 3000; i += 2) ASSERT_TRUE(db->Delete(Key(i)).ok());
  ASSERT_TRUE(db->Flush().ok());
  uint64_t before_bytes = db->GetStats().table_bytes;
  EXPECT_GT(db->GetStats().num_tables, 5u);
  ASSERT_TRUE(db->CompactAll().ok());
  DBStats st = db->GetStats();
  EXPECT_EQ(st.num_tables, 1u);
  EXPECT_EQ(st.tombstones_dropped, 1500u);
  EXPECT_LT(st.table_bytes, before_bytes / 3);  // 1500 live keys out of 7500 entries
  for (int i = 0; i < 3000; i++) {
    ASSERT_EQ(GetOrMissing(db.get(), Key(i)), i % 2 ? std::string(50, 'b') : "<missing>");
  }
  // Delete everything and compact: no tables left at all.
  for (int i = 1; i < 3000; i += 2) ASSERT_TRUE(db->Delete(Key(i)).ok());
  ASSERT_TRUE(db->CompactAll().ok());
  EXPECT_EQ(db->GetStats().num_tables, 0u);
  EXPECT_EQ(CountSst(dir.path()), 0u);
  std::vector<std::pair<std::string, std::string>> got;
  ASSERT_TRUE(db->Scan("", 100, &got).ok());
  EXPECT_TRUE(got.empty());
}

// A compaction that does not include the oldest table must keep tombstones:
// the oldest table still holds the deleted value.
TEST(CompactionTest, TombstoneKeptWhenOlderTableExists) {
  test::TempDir dir;
  std::unique_ptr<DB> db;
  Options o;  // large memtable: tables are created only by the Flush() calls below
  ASSERT_TRUE(DB::Open(o, dir.path(), &db).ok());
  // Oldest table: big, so it is not in the same size tier as what follows.
  for (int i = 0; i < 2000; i++) ASSERT_TRUE(db->Put(Key(i), std::string(100, 'x')).ok());
  ASSERT_TRUE(db->Flush().ok());
  ASSERT_TRUE(db->WaitForIdle().ok());
  ASSERT_EQ(db->GetStats().num_tables, 1u);
  // Four tiny tables, one of them holding a tombstone for key 7.
  ASSERT_TRUE(db->Delete(Key(7)).ok());
  ASSERT_TRUE(db->Flush().ok());
  for (int t = 0; t < 3; t++) {
    ASSERT_TRUE(db->Put("tiny" + std::to_string(t), "v").ok());
    ASSERT_TRUE(db->Flush().ok());
  }
  ASSERT_TRUE(db->WaitForIdle().ok());
  DBStats st = db->GetStats();
  EXPECT_EQ(st.compactions, 1u);
  EXPECT_EQ(st.num_tables, 2u);  // big old table + merged tiny one
  EXPECT_EQ(st.tombstones_dropped, 0u);
  EXPECT_EQ(GetOrMissing(db.get(), Key(7)), "<missing>");
  EXPECT_EQ(GetOrMissing(db.get(), Key(8)), std::string(100, 'x'));
}

// Writers, readers, flushes and compactions all at once (run under TSan).
TEST(CompactionTest, ConcurrentLoadWithCompaction) {
  test::TempDir dir;
  std::unique_ptr<DB> db;
  Options o = SmallOptions();
  o.stall_table_count = 8;  // also exercise the table-count write stall
  ASSERT_TRUE(DB::Open(o, dir.path(), &db).ok());
  constexpr int kWriters = 3, kPerWriter = 6000;
  std::atomic<int> done{0}, errors{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; w++) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; i++) {
        int k = w * kPerWriter + i;
        if (!db->Put(Key(k), std::to_string(k) + std::string(40, 'p')).ok()) errors++;
      }
      done++;
    });
  }
  for (int r = 0; r < 2; r++) {
    threads.emplace_back([&, r] {
      std::mt19937 rng(r + 50);
      while (done.load() < kWriters) {
        int k = static_cast<int>(rng() % (kWriters * kPerWriter));
        std::string v;
        Status s = db->Get(Key(k), &v);
        if (s.ok() && v != std::to_string(k) + std::string(40, 'p')) errors++;
        if (!s.ok() && !s.IsNotFound()) errors++;
        std::vector<std::pair<std::string, std::string>> out;
        if (!db->Scan(Key(k), 50, &out).ok()) errors++;
        for (size_t i = 1; i < out.size(); i++) errors += out[i - 1].first >= out[i].first;
      }
    });
  }
  for (auto& t : threads) t.join();
  ASSERT_TRUE(db->WaitForIdle().ok());
  EXPECT_EQ(errors.load(), 0);
  DBStats st = db->GetStats();
  EXPECT_GT(st.compactions, 5u);
  EXPECT_LT(st.num_tables, o.stall_table_count);
  for (int k = 0; k < kWriters * kPerWriter; k++) {
    ASSERT_EQ(GetOrMissing(db.get(), Key(k)), std::to_string(k) + std::string(40, 'p'));
  }
}

// Closing while compaction is busy must not lose or corrupt anything.
TEST(CompactionTest, CloseDuringCompactionThenReopen) {
  test::TempDir dir;
  for (int round = 0; round < 3; round++) {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(SmallOptions(), dir.path(), &db).ok());
    for (int i = 0; i < 8000; i++) {
      ASSERT_TRUE(db->Put(Key(round * 8000 + i), std::to_string(i)).ok());
    }
    // Destroy right away: a compaction is very likely still running.
  }
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(SmallOptions(), dir.path(), &db).ok());
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 8000; i++) {
      ASSERT_EQ(GetOrMissing(db.get(), Key(round * 8000 + i)), std::to_string(i));
    }
  }
}

}  // namespace
}  // namespace lsm
