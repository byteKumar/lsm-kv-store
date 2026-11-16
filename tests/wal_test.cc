#include "wal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "memtable.h"
#include "test_util.h"

namespace lsm {
namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

void WriteFile(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::vector<std::string> ReadAll(const std::string& path, WalRecoveryResult* r) {
  std::vector<std::string> got;
  Status s = ReadWal(
      path,
      [&](std::string_view p) {
        got.emplace_back(p);
        return Status::OK();
      },
      r);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return got;
}

// Writes records "record-0", "record-1", ... and returns their end offsets.
std::vector<uint64_t> WriteRecords(const std::string& path, int n, SyncMode mode = SyncMode::kNone) {
  std::unique_ptr<WalWriter> w;
  EXPECT_TRUE(WalWriter::Open(path, mode, 0, &w).ok());
  std::vector<uint64_t> ends;
  for (int i = 0; i < n; i++) {
    EXPECT_TRUE(w->AddRecord("record-" + std::to_string(i)).ok());
    ends.push_back(w->FileSize());
  }
  EXPECT_TRUE(w->Close().ok());
  return ends;
}

TEST(WalTest, WriteThenReadBack) {
  test::TempDir dir;
  std::string path = dir.File("wal.log");
  auto ends = WriteRecords(path, 100);
  WalRecoveryResult r;
  auto got = ReadAll(path, &r);
  ASSERT_EQ(got.size(), 100u);
  for (int i = 0; i < 100; i++) EXPECT_EQ(got[i], "record-" + std::to_string(i));
  EXPECT_EQ(r.records, 100u);
  EXPECT_EQ(r.valid_bytes, ends.back());
  EXPECT_EQ(r.file_bytes, ends.back());
  EXPECT_FALSE(r.tail_dropped);
}

TEST(WalTest, MissingFileIsEmpty) {
  test::TempDir dir;
  WalRecoveryResult r;
  auto got = ReadAll(dir.File("nope.log"), &r);
  EXPECT_TRUE(got.empty());
  EXPECT_EQ(r.records, 0u);
}

TEST(WalTest, EmptyAndLargePayloads) {
  test::TempDir dir;
  std::string path = dir.File("wal.log");
  std::string big(1 << 20, 'x');
  {
    std::unique_ptr<WalWriter> w;
    ASSERT_TRUE(WalWriter::Open(path, SyncMode::kNone, 0, &w).ok());
    ASSERT_TRUE(w->AddRecord("").ok());
    ASSERT_TRUE(w->AddRecord(big).ok());
  }
  WalRecoveryResult r;
  auto got = ReadAll(path, &r);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], "");
  EXPECT_EQ(got[1], big);
}

// Simulates a crash at every possible byte: cutting the file anywhere must
// give back exactly the records that were completely written before the cut.
TEST(WalTest, TruncationAtEveryByteKeepsCompleteRecords) {
  test::TempDir dir;
  std::string path = dir.File("wal.log");
  auto ends = WriteRecords(path, 20);
  std::string full = ReadFile(path);
  std::string cut_path = dir.File("cut.log");
  for (size_t cut = 0; cut <= full.size(); cut++) {
    WriteFile(cut_path, full.substr(0, cut));
    size_t expected = 0;
    while (expected < ends.size() && ends[expected] <= cut) expected++;
    WalRecoveryResult r;
    auto got = ReadAll(cut_path, &r);
    ASSERT_EQ(got.size(), expected) << "cut=" << cut;
    EXPECT_EQ(r.valid_bytes, expected == 0 ? 0 : ends[expected - 1]);
    EXPECT_EQ(r.tail_dropped, r.valid_bytes != cut) << "cut=" << cut;
  }
}

TEST(WalTest, BitFlipInLastRecordDropsOnlyIt) {
  test::TempDir dir;
  std::string path = dir.File("wal.log");
  auto ends = WriteRecords(path, 10);
  std::string data = ReadFile(path);
  data[data.size() - 2] ^= 0x01;  // inside the last payload
  WriteFile(path, data);
  WalRecoveryResult r;
  auto got = ReadAll(path, &r);
  EXPECT_EQ(got.size(), 9u);
  EXPECT_EQ(r.valid_bytes, ends[8]);
  EXPECT_TRUE(r.tail_dropped);
  EXPECT_NE(r.stop_reason.find("checksum"), std::string::npos) << r.stop_reason;
}

TEST(WalTest, CorruptLengthFieldIsCaught) {
  test::TempDir dir;
  std::string path = dir.File("wal.log");
  auto ends = WriteRecords(path, 5);
  std::string data = ReadFile(path);
  // Change record 3's length by one: still "fits" in the file, so only the
  // CRC (which covers the length) can catch it.
  size_t off = ends[2] + 4;
  data[off] = static_cast<char>(data[off] - 1);
  WriteFile(path, data);
  WalRecoveryResult r;
  auto got = ReadAll(path, &r);
  EXPECT_EQ(got.size(), 3u);
  EXPECT_EQ(r.valid_bytes, ends[2]);
}

// Corruption in the middle stops replay there. This is the "trust nothing
// after the first bad record" policy; the records after it are lost.
TEST(WalTest, CorruptionInMiddleStopsReplay) {
  test::TempDir dir;
  std::string path = dir.File("wal.log");
  auto ends = WriteRecords(path, 10);
  std::string data = ReadFile(path);
  data[ends[4] + 10] ^= 0x40;  // inside record 5's payload
  WriteFile(path, data);
  WalRecoveryResult r;
  auto got = ReadAll(path, &r);
  EXPECT_EQ(got.size(), 5u);
  EXPECT_TRUE(r.tail_dropped);
}

// After a torn tail, reopening at valid_bytes must cut the garbage off so
// new records are readable next time.
TEST(WalTest, ReopenAfterTornTailAppendsCleanly) {
  test::TempDir dir;
  std::string path = dir.File("wal.log");
  WriteRecords(path, 5);
  std::string data = ReadFile(path);
  data += std::string("\x12\x34\x56\x78\x20\x00", 6);  // half a header
  WriteFile(path, data);

  WalRecoveryResult r;
  ASSERT_EQ(ReadAll(path, &r).size(), 5u);
  ASSERT_TRUE(r.tail_dropped);
  {
    std::unique_ptr<WalWriter> w;
    ASSERT_TRUE(WalWriter::Open(path, SyncMode::kNone, r.valid_bytes, &w).ok());
    for (int i = 5; i < 8; i++) ASSERT_TRUE(w->AddRecord("record-" + std::to_string(i)).ok());
  }
  auto got = ReadAll(path, &r);
  ASSERT_EQ(got.size(), 8u);
  EXPECT_EQ(got[7], "record-7");
  EXPECT_FALSE(r.tail_dropped);
}

TEST(WalTest, AllSyncModesWrite) {
  for (SyncMode m : {SyncMode::kNone, SyncMode::kFsync, SyncMode::kFullFsync}) {
    test::TempDir dir;
    std::string path = dir.File("wal.log");
    WriteRecords(path, 3, m);
    WalRecoveryResult r;
    EXPECT_EQ(ReadAll(path, &r).size(), 3u) << SyncModeName(m);
  }
  SyncMode m;
  EXPECT_TRUE(ParseSyncMode("full", &m));
  EXPECT_EQ(m, SyncMode::kFullFsync);
  EXPECT_FALSE(ParseSyncMode("sometimes", &m));
}

TEST(WalEntryTest, EncodeDecodeRoundTrip) {
  std::string p = EncodeWalEntry(42, ValueType::kValue, std::string("k\0y", 3), "value");
  SequenceNumber seq;
  ValueType type;
  std::string_view k, v;
  ASSERT_TRUE(DecodeWalEntry(p, &seq, &type, &k, &v));
  EXPECT_EQ(seq, 42u);
  EXPECT_EQ(type, ValueType::kValue);
  EXPECT_EQ(k, std::string_view("k\0y", 3));
  EXPECT_EQ(v, "value");

  EXPECT_FALSE(DecodeWalEntry(p + "x", &seq, &type, &k, &v));        // trailing bytes
  EXPECT_FALSE(DecodeWalEntry(p.substr(0, p.size() - 1), &seq, &type, &k, &v));
  std::string bad_type = p;
  bad_type[8] = 7;
  EXPECT_FALSE(DecodeWalEntry(bad_type, &seq, &type, &k, &v));
}

TEST(WalRecoveryTest, ReplayRebuildsMemTable) {
  test::TempDir dir;
  std::string path = dir.File("wal.log");
  {
    std::unique_ptr<WalWriter> w;
    ASSERT_TRUE(WalWriter::Open(path, SyncMode::kNone, 0, &w).ok());
    ASSERT_TRUE(w->AddRecord(EncodeWalEntry(1, ValueType::kValue, "a", "1")).ok());
    ASSERT_TRUE(w->AddRecord(EncodeWalEntry(2, ValueType::kValue, "b", "2")).ok());
    ASSERT_TRUE(w->AddRecord(EncodeWalEntry(3, ValueType::kDeletion, "a", "")).ok());
    ASSERT_TRUE(w->AddRecord(EncodeWalEntry(4, ValueType::kValue, "b", "22")).ok());
  }
  MemTable mem;
  SequenceNumber max_seq = 0;
  WalRecoveryResult r;
  ASSERT_TRUE(RecoverMemTable(path, &mem, &max_seq, &r).ok());
  EXPECT_EQ(r.records, 4u);
  EXPECT_EQ(max_seq, 4u);
  std::string v;
  EXPECT_EQ(mem.Get("a", &v), MemTable::GetResult::kDeleted);
  ASSERT_EQ(mem.Get("b", &v), MemTable::GetResult::kFound);
  EXPECT_EQ(v, "22");
}

TEST(WalRecoveryTest, UnparseablePayloadWithGoodCrcIsCorruption) {
  test::TempDir dir;
  std::string path = dir.File("wal.log");
  {
    std::unique_ptr<WalWriter> w;
    ASSERT_TRUE(WalWriter::Open(path, SyncMode::kNone, 0, &w).ok());
    ASSERT_TRUE(w->AddRecord("not an entry").ok());
  }
  MemTable mem;
  SequenceNumber max_seq = 0;
  WalRecoveryResult r;
  EXPECT_TRUE(RecoverMemTable(path, &mem, &max_seq, &r).IsCorruption());
}

}  // namespace
}  // namespace lsm
