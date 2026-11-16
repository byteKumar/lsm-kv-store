#pragma once

// The storage engine: ties together the WAL, memtables, SSTables and the
// manifest.
//
// Write path:  Put/Delete -> append to the WAL (synced per SyncMode) -> insert
//              into the active memtable. When the memtable is full it is frozen
//              ("immutable memtable"), a fresh memtable and WAL take over, and
//              the background thread writes the frozen one out as an SSTable.
// Read path:   active memtable -> immutable memtable -> SSTables newest to
//              oldest. The first version found wins; a tombstone means "deleted".
// Recovery:    read MANIFEST, open its tables, replay any WAL files that are
//              not yet in a table, write that data to a new table, start a
//              fresh WAL.
//
// Threading:   any number of threads may call Put/Delete/Get/NewIterator.
//              Writes are serialized by write_mu_. Readers take a short lock on
//              mu_ to copy shared pointers to the current memtables and table
//              list, then read without holding any lock.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "iterator.h"
#include "manifest.h"
#include "memtable.h"
#include "sstable.h"
#include "status.h"
#include "wal.h"

namespace lsm {

struct Options {
  // Applies to the WAL (every write), new tables and the manifest.
  SyncMode sync_mode = SyncMode::kNone;
  size_t memtable_size = 64 << 20;  // flush when the active memtable passes this
  size_t block_size = 4096;
  int bloom_bits_per_key = 10;  // 0 = no Bloom filters in new tables
  bool use_bloom = true;        // false = ignore filters when reading
};

struct DBStats {
  uint64_t user_bytes_written = 0;  // key + value bytes passed to Put/Delete
  uint64_t wal_bytes = 0;
  uint64_t flush_bytes = 0;         // SSTable bytes written by memtable flushes
  uint64_t compaction_bytes = 0;    // SSTable bytes written by compaction (milestone 6)
  uint64_t manifest_bytes = 0;
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  uint64_t write_stalls = 0;        // writes that waited for a flush to finish
  uint64_t num_tables = 0;
  uint64_t table_bytes = 0;         // total size of live tables
  uint64_t block_reads = 0;
  uint64_t bloom_checks = 0;
  uint64_t bloom_negatives = 0;
  uint64_t bloom_false_positives = 0;

  uint64_t DiskBytesWritten() const {
    return wal_bytes + flush_bytes + compaction_bytes + manifest_bytes;
  }
};

class DB {
 public:
  static Status Open(const Options& options, const std::string& dir, std::unique_ptr<DB>* out);
  // Waits for an in-progress flush, then stops the background thread. Data
  // still in the active memtable is safe in the WAL and replayed on next open.
  ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // Returns once the write is in the WAL (and synced, per sync_mode).
  Status Put(std::string_view key, std::string_view value);
  Status Delete(std::string_view key);

  // NotFound if the key does not exist or was deleted.
  Status Get(std::string_view key, std::string* value);

  // Point-in-time view: writes made after this call are not visible.
  std::unique_ptr<Iterator> NewIterator();

  // Up to `limit` key/value pairs with key >= start, in key order.
  Status Scan(std::string_view start, size_t limit,
              std::vector<std::pair<std::string, std::string>>* out);

  // Writes the active memtable to an SSTable and waits for it.
  Status Flush();

  DBStats GetStats() const;

 private:
  struct Version;  // memtables + table list that a reader snapshots
  struct TableHandle;

  DB(const Options& options, std::string dir);
  Status Recover();
  Status Write(ValueType type, std::string_view key, std::string_view value);
  // Freezes the active memtable and switches to a new WAL. Caller holds
  // write_mu_; waits if a previous frozen memtable is still being flushed.
  Status SwitchMemTable(std::unique_lock<std::mutex>* lock);
  std::shared_ptr<const Version> CurrentVersion() const;
  void BackgroundThread();
  Status FlushImmutable();
  // Writes the newest version of every key from `it` into table `number`.
  Status WriteTable(Iterator* it, uint64_t number, uint64_t* file_size,
                    std::shared_ptr<TableHandle>* handle);
  Status OpenTable(uint64_t number, std::shared_ptr<TableHandle>* handle);
  void DeleteObsoleteFiles();
  ManifestState MakeManifestState(const std::vector<std::shared_ptr<TableHandle>>& tables,
                                  uint64_t log_number) const;

  const Options options_;
  const std::string dir_;
  int lock_fd_ = -1;

  std::mutex write_mu_;  // serializes writers (WAL append + memtable insert)

  mutable std::mutex mu_;  // guards everything below
  std::condition_variable bg_cv_;    // wakes the background thread
  std::condition_variable done_cv_;  // signals a finished flush
  std::shared_ptr<const Version> version_;
  std::unique_ptr<WalWriter> wal_;
  uint64_t log_number_ = 0;      // WAL of the active memtable
  uint64_t imm_log_number_ = 0;  // WAL of the immutable memtable, 0 if none
  uint64_t next_file_number_ = 1;
  Status bg_error_;
  bool shutting_down_ = false;

  std::atomic<SequenceNumber> last_seq_{0};     // last sequence number handed out
  std::atomic<SequenceNumber> visible_seq_{0};  // last write fully in the memtable

  ReadStats read_stats_;
  std::atomic<uint64_t> user_bytes_{0}, wal_bytes_{0}, flush_bytes_{0}, compaction_bytes_{0},
      manifest_bytes_{0}, flushes_{0}, compactions_{0}, write_stalls_{0};

  std::thread bg_thread_;
};

}  // namespace lsm
