#include "db.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "manifest.h"
#include "test_util.h"

namespace lsm {
namespace {

namespace fs = std::filesystem;

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%010d", i);
  return buf;
}

Options SmallOptions() {
  Options o;
  o.memtable_size = 32 << 10;  // 32 KB: forces many flushes in small tests
  return o;
}

std::unique_ptr<DB> OpenDB(const std::string& dir, const Options& o = SmallOptions()) {
  std::unique_ptr<DB> db;
  Status s = DB::Open(o, dir, &db);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return db;
}

std::string GetOrMissing(DB* db, std::string_view key) {
  std::string v;
  Status s = db->Get(key, &v);
  if (s.IsNotFound()) return "<missing>";
  EXPECT_TRUE(s.ok()) << s.ToString();
  return v;
}

size_t CountFiles(const std::string& dir, const std::string& ext) {
  size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() == ext) n++;
  }
  return n;
}

TEST(DBTest, PutGetDeleteInMemTable) {
  test::TempDir dir;
  auto db = OpenDB(dir.path());
  ASSERT_TRUE(db->Put("a", "1").ok());
  ASSERT_TRUE(db->Put("b", "2").ok());
  EXPECT_EQ(GetOrMissing(db.get(), "a"), "1");
  ASSERT_TRUE(db->Put("a", "11").ok());
  EXPECT_EQ(GetOrMissing(db.get(), "a"), "11");
  ASSERT_TRUE(db->Delete("a").ok());
  EXPECT_EQ(GetOrMissing(db.get(), "a"), "<missing>");
  EXPECT_EQ(GetOrMissing(db.get(), "zzz"), "<missing>");
}

TEST(DBTest, ReadsAcrossMemTableAndTables) {
  test::TempDir dir;
  auto db = OpenDB(dir.path());
  ASSERT_TRUE(db->Put("k1", "old").ok());
  ASSERT_TRUE(db->Put("k2", "keep").ok());
  ASSERT_TRUE(db->Flush().ok());
  EXPECT_EQ(db->GetStats().num_tables, 1u);
  ASSERT_TRUE(db->Put("k1", "new").ok());  // memtable shadows table
  ASSERT_TRUE(db->Delete("k2").ok());      // tombstone in memtable hides table value
  EXPECT_EQ(GetOrMissing(db.get(), "k1"), "new");
  EXPECT_EQ(GetOrMissing(db.get(), "k2"), "<missing>");
  ASSERT_TRUE(db->Flush().ok());
  EXPECT_EQ(db->GetStats().num_tables, 2u);
  // Now both versions are in tables: the newer table must win.
  EXPECT_EQ(GetOrMissing(db.get(), "k1"), "new");
  EXPECT_EQ(GetOrMissing(db.get(), "k2"), "<missing>");
}

// Random puts/deletes with a tiny memtable (many flushes, write stalls),
// checked against std::map, then again after reopening.
TEST(DBTest, RandomOpsMatchModelAcrossFlushesAndReopen) {
  test::TempDir dir;
  std::map<std::string, std::optional<std::string>> model;
  std::mt19937_64 rng(2024);
  {
    auto db = OpenDB(dir.path());
    for (int i = 0; i < 30000; i++) {
      std::string k = Key(static_cast<int>(rng() % 4000));
      if (rng() % 5 == 0) {
        ASSERT_TRUE(db->Delete(k).ok());
        model[k] = std::nullopt;
      } else {
        std::string v = "v" + std::to_string(i) + std::string(rng() % 100, 'x');
        ASSERT_TRUE(db->Put(k, v).ok());
        model[k] = v;
      }
    }
    DBStats st = db->GetStats();
    EXPECT_GT(st.flushes, 20u);
    EXPECT_GT(st.num_tables, 20u);
    for (int i = 0; i < 4100; i++) {
      auto it = model.find(Key(i));
      std::string want = (it == model.end() || !it->second) ? "<missing>" : *it->second;
      ASSERT_EQ(GetOrMissing(db.get(), Key(i)), want) << i;
    }
  }
  auto db = OpenDB(dir.path());
  for (int i = 0; i < 4100; i++) {
    auto it = model.find(Key(i));
    std::string want = (it == model.end() || !it->second) ? "<missing>" : *it->second;
    ASSERT_EQ(GetOrMissing(db.get(), Key(i)), want) << i;
  }
}

TEST(DBTest, ReopenReplaysWalIntoTableAndRemovesOldLogs) {
  test::TempDir dir;
  {
    Options o;  // big memtable: nothing is flushed before close
    auto db = OpenDB(dir.path(), o);
    for (int i = 0; i < 100; i++) ASSERT_TRUE(db->Put(Key(i), "v" + std::to_string(i)).ok());
    ASSERT_TRUE(db->Delete(Key(5)).ok());
    EXPECT_EQ(db->GetStats().num_tables, 0u);
  }
  auto db = OpenDB(dir.path(), Options{});
  EXPECT_EQ(db->GetStats().num_tables, 1u);  // replayed WAL became a table
  EXPECT_EQ(CountFiles(dir.path(), ".log"), 1u);  // just the new, empty log
  EXPECT_EQ(GetOrMissing(db.get(), Key(7)), "v7");
  EXPECT_EQ(GetOrMissing(db.get(), Key(5)), "<missing>");
  // New writes get larger sequence numbers than recovered ones.
  ASSERT_TRUE(db->Put(Key(7), "newer").ok());
  EXPECT_EQ(GetOrMissing(db.get(), Key(7)), "newer");
}

// Copying the directory while the DB is open captures what a kill -9 would
// leave behind: everything written with write(2), nothing from destructors.
TEST(DBTest, CrashSnapshotRecoversAllAcknowledgedWrites) {
  test::TempDir dir, copy;
  auto db = OpenDB(dir.path());
  for (int i = 0; i < 5000; i++) ASSERT_TRUE(db->Put(Key(i), std::to_string(i)).ok());
  // Wait for background flushes so the copy (which is not atomic across
  // files) cannot race with one. The last writes stay in the memtable and
  // exist only in the WAL. The real kill -9 test (milestone 8) covers crashes
  // in the middle of a flush.
  ASSERT_TRUE(db->Flush().ok());
  for (int i = 5000; i < 5100; i++) ASSERT_TRUE(db->Put(Key(i), std::to_string(i)).ok());
  ASSERT_EQ(db->GetStats().flushes, db->GetStats().num_tables);
  fs::copy(dir.path(), copy.path(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
  fs::remove(LockFileName(copy.path()));  // the copy is a different file: drop the stale lock file
  auto recovered = OpenDB(copy.path());
  for (int i = 0; i < 5100; i++) ASSERT_EQ(GetOrMissing(recovered.get(), Key(i)), std::to_string(i));
}

TEST(DBTest, TornWalTailIsIgnoredOnRecovery) {
  test::TempDir dir;
  {
    Options o;
    auto db = OpenDB(dir.path(), o);
    for (int i = 0; i < 50; i++) ASSERT_TRUE(db->Put(Key(i), "v").ok());
  }
  // Find the live log and append half a record to it.
  std::string log;
  for (const auto& e : fs::directory_iterator(dir.path())) {
    if (e.path().extension() == ".log" && fs::file_size(e.path()) > 0) log = e.path().string();
  }
  ASSERT_FALSE(log.empty());
  {
    std::ofstream out(log, std::ios::binary | std::ios::app);
    out.write("\x01\x02\x03\x04\x50\x00\x00\x00partial", 15);
  }
  auto db = OpenDB(dir.path(), Options{});
  for (int i = 0; i < 50; i++) ASSERT_EQ(GetOrMissing(db.get(), Key(i)), "v");
}

TEST(DBTest, ScanMergesAllSourcesAndHidesDeletes) {
  test::TempDir dir;
  auto db = OpenDB(dir.path());
  std::map<std::string, std::optional<std::string>> model;
  std::mt19937_64 rng(5);
  for (int round = 0; round < 4; round++) {
    for (int i = 0; i < 3000; i++) {
      std::string k = Key(static_cast<int>(rng() % 2000));
      if (rng() % 4 == 0) {
        ASSERT_TRUE(db->Delete(k).ok());
        model[k] = std::nullopt;
      } else {
        std::string v = "r" + std::to_string(round) + "-" + std::to_string(i);
        ASSERT_TRUE(db->Put(k, v).ok());
        model[k] = v;
      }
    }
    if (round < 3) ASSERT_TRUE(db->Flush().ok());  // last round stays in the memtable
  }
  std::vector<std::pair<std::string, std::string>> live;
  for (const auto& [k, v] : model) {
    if (v) live.emplace_back(k, *v);
  }
  // Full scan equals the live part of the model.
  std::vector<std::pair<std::string, std::string>> got;
  ASSERT_TRUE(db->Scan("", 1000000, &got).ok());
  EXPECT_EQ(got, live);
  // Bounded scans from random starts.
  for (int t = 0; t < 50; t++) {
    std::string start = Key(static_cast<int>(rng() % 2100));
    ASSERT_TRUE(db->Scan(start, 100, &got).ok());
    auto it = std::lower_bound(live.begin(), live.end(), std::make_pair(start, std::string()));
    std::vector<std::pair<std::string, std::string>> want(
        it, it + std::min<ptrdiff_t>(100, live.end() - it));
    ASSERT_EQ(got, want) << start;
  }
}

TEST(DBTest, IteratorIsPointInTime) {
  test::TempDir dir;
  auto db = OpenDB(dir.path(), Options{});
  ASSERT_TRUE(db->Put("a", "1").ok());
  ASSERT_TRUE(db->Put("c", "3").ok());
  auto it = db->NewIterator();
  ASSERT_TRUE(db->Put("b", "2").ok());   // after the iterator: invisible
  ASSERT_TRUE(db->Put("a", "11").ok());  // newer version: iterator still sees "1"
  ASSERT_TRUE(db->Delete("c").ok());     // delete after: iterator still sees "c"
  std::vector<std::pair<std::string, std::string>> got;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    got.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  std::vector<std::pair<std::string, std::string>> want = {{"a", "1"}, {"c", "3"}};
  EXPECT_EQ(got, want);
}

TEST(DBTest, SecondOpenOfSameDirectoryFails) {
  test::TempDir dir;
  auto db = OpenDB(dir.path());
  std::unique_ptr<DB> other;
  Status s = DB::Open(SmallOptions(), dir.path(), &other);
  EXPECT_FALSE(s.ok());
  EXPECT_NE(s.message().find("locked"), std::string::npos) << s.ToString();
}

TEST(DBTest, OrphanFilesAreRemovedOnOpen) {
  test::TempDir dir;
  { auto db = OpenDB(dir.path()); ASSERT_TRUE(db->Put("a", "1").ok()); }
  { std::ofstream(dir.File("999999.sst")) << "half-written table"; }
  { std::ofstream(dir.File("MANIFEST.tmp")) << "junk"; }
  auto db = OpenDB(dir.path());
  EXPECT_FALSE(fs::exists(dir.File("999999.sst")));
  EXPECT_FALSE(fs::exists(dir.File("MANIFEST.tmp")));
  EXPECT_EQ(GetOrMissing(db.get(), "a"), "1");
}

TEST(DBTest, MissingTableOrBadManifestFailsOpen) {
  test::TempDir dir;
  {
    auto db = OpenDB(dir.path());
    ASSERT_TRUE(db->Put("a", "1").ok());
    ASSERT_TRUE(db->Flush().ok());
  }
  ManifestState st;
  ASSERT_TRUE(ReadManifest(dir.path(), &st).ok());
  ASSERT_EQ(st.tables.size(), 1u);
  std::string table = TableFileName(dir.path(), st.tables[0]);
  fs::rename(table, table + ".bak");
  std::unique_ptr<DB> db;
  EXPECT_FALSE(DB::Open(SmallOptions(), dir.path(), &db).ok());
  fs::rename(table + ".bak", table);

  {
    std::fstream f(ManifestFileName(dir.path()), std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(12);
    f.put('\x7f');
  }
  Status s = DB::Open(SmallOptions(), dir.path(), &db);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(DBTest, StatsCountBytes) {
  test::TempDir dir;
  auto db = OpenDB(dir.path());
  for (int i = 0; i < 2000; i++) ASSERT_TRUE(db->Put(Key(i), std::string(100, 'v')).ok());
  ASSERT_TRUE(db->Flush().ok());
  DBStats st = db->GetStats();
  EXPECT_EQ(st.user_bytes_written, 2000u * (13 + 100));
  EXPECT_GT(st.wal_bytes, st.user_bytes_written);  // headers + seq + lengths
  EXPECT_GT(st.flush_bytes, st.user_bytes_written);
  EXPECT_EQ(st.table_bytes, st.flush_bytes);  // no compaction yet
}

TEST(DBTest, BlockReadsAndBloomStats) {
  test::TempDir dir;
  auto db = OpenDB(dir.path());
  for (int i = 0; i < 2000; i += 2) ASSERT_TRUE(db->Put(Key(i), "v").ok());
  ASSERT_TRUE(db->Flush().ok());
  DBStats before = db->GetStats();
  for (int i = 1; i < 2000; i += 2) EXPECT_EQ(GetOrMissing(db.get(), Key(i)), "<missing>");
  DBStats after = db->GetStats();
  EXPECT_GT(after.bloom_negatives - before.bloom_negatives, 900u);
  EXPECT_EQ(after.block_reads - before.block_reads,
            after.bloom_false_positives - before.bloom_false_positives);
}

// Several writers and readers at once while flushes run (run under TSan).
TEST(DBTest, ConcurrentWritersAndReaders) {
  test::TempDir dir;
  auto db = OpenDB(dir.path());
  constexpr int kWriters = 4, kPerWriter = 3000;
  std::atomic<int> done_writers{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; w++) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; i++) {
        int k = w * kPerWriter + i;
        if (!db->Put(Key(k), std::to_string(k)).ok()) errors++;
      }
      done_writers++;
    });
  }
  for (int r = 0; r < 3; r++) {
    threads.emplace_back([&, r] {
      std::mt19937 rng(r);
      while (done_writers.load() < kWriters) {
        int k = static_cast<int>(rng() % (kWriters * kPerWriter));
        std::string v;
        Status s = db->Get(Key(k), &v);
        if (s.ok() && v != std::to_string(k)) errors++;
        if (!s.ok() && !s.IsNotFound()) errors++;
        std::vector<std::pair<std::string, std::string>> out;
        if (!db->Scan(Key(k), 20, &out).ok()) errors++;
        for (size_t i = 1; i < out.size(); i++) {
          if (out[i - 1].first >= out[i].first) errors++;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(errors.load(), 0);
  for (int k = 0; k < kWriters * kPerWriter; k++) {
    ASSERT_EQ(GetOrMissing(db.get(), Key(k)), std::to_string(k));
  }
  EXPECT_GT(db->GetStats().flushes, 5u);
}

TEST(DBTest, AllSyncModesWork) {
  for (SyncMode m : {SyncMode::kNone, SyncMode::kFsync, SyncMode::kFullFsync}) {
    test::TempDir dir;
    Options o = SmallOptions();
    o.sync_mode = m;
    {
      auto db = OpenDB(dir.path(), o);
      for (int i = 0; i < 300; i++) ASSERT_TRUE(db->Put(Key(i), "v").ok());
    }
    auto db = OpenDB(dir.path(), o);
    EXPECT_EQ(GetOrMissing(db.get(), Key(299)), "v") << SyncModeName(m);
  }
}

}  // namespace
}  // namespace lsm
