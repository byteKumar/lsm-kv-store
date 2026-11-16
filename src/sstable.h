#pragma once

// Sorted String Table: an immutable, sorted file of entries.
//
// File layout:
//
//   [data block 0][crc]
//   [data block 1][crc]
//   ...
//   [index block][crc]      one entry per data block: its last key + seq,
//                           offset and size (a "sparse" index)
//   [filter block][crc]     Bloom filter over the table's keys (empty if off)
//   [props block][crc]      smallest/largest key, entry/key counts, seq range
//   [footer, 56 bytes]      offsets and sizes of index/filter/props + magic
//
// Each data block holds entries back to back, ~4 KB per block:
//   [key_len u32][key][seq u64][type u8][value_len u32][value]
//
// A point lookup first checks the key range and the Bloom filter (both in
// memory). Only if the filter says "maybe" does it binary-search the
// in-memory index for the single block that could hold the key and read that
// one block from disk.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bloom.h"
#include "iterator.h"
#include "status.h"
#include "types.h"
#include "wal.h"  // SyncMode

namespace lsm {

struct SSTableOptions {
  size_t block_size = 4096;
  int bloom_bits_per_key = 10;  // 0 = write no filter
};

// Read counters. Each table keeps its own; a database can also pass one
// shared set so totals survive tables being deleted by compaction.
struct ReadStats {
  std::atomic<uint64_t> block_reads{0};            // data blocks read from disk
  std::atomic<uint64_t> bloom_checks{0};           // Get() consulted the filter
  std::atomic<uint64_t> bloom_negatives{0};        // filter said "no": read skipped
  std::atomic<uint64_t> bloom_false_positives{0};  // filter said "maybe", key absent
};

struct SSTableReadOptions {
  bool use_bloom = true;               // false = ignore the filter (for A/B measurements)
  ReadStats* shared_stats = nullptr;   // also count into this, if set
};

class SSTableWriter {
 public:
  ~SSTableWriter();
  static Status Open(const std::string& path, const SSTableOptions& opts,
                     std::unique_ptr<SSTableWriter>* out);

  // Entries must arrive in strictly increasing internal order.
  Status Add(std::string_view key, SequenceNumber seq, ValueType type, std::string_view value);

  // Writes index, filter, props and footer, syncs per mode, closes the file.
  Status Finish(SyncMode mode);

  uint64_t NumEntries() const { return num_entries_; }
  uint64_t FileSize() const { return offset_; }

 private:
  SSTableWriter(int fd, const SSTableOptions& opts) : fd_(fd), opts_(opts) {}
  Status FlushBlock();
  Status WriteBlockWithCrc(const std::string& contents, uint64_t* offset, uint64_t* size);

  int fd_;
  SSTableOptions opts_;
  std::string block_;      // data block being built
  std::string index_;      // index entries for finished blocks
  std::string last_key_;   // last key added (for order checks and index)
  SequenceNumber last_seq_ = 0;
  std::string smallest_key_;
  SequenceNumber min_seq_ = kMaxSequenceNumber, max_seq_ = 0;
  uint64_t num_entries_ = 0;
  uint64_t num_keys_ = 0;  // distinct user keys
  std::unique_ptr<BloomFilterBuilder> bloom_;
  uint64_t offset_ = 0;  // bytes written so far
  Status status_;
  bool finished_ = false;
};

class SSTableReader {
 public:
  enum class GetResult { kNotFound, kFound, kDeleted };

  ~SSTableReader();
  static Status Open(const std::string& path, std::unique_ptr<SSTableReader>* out,
                     SSTableReadOptions opts = {});

  // Finds the newest version of key in this table. Thread-safe.
  Status Get(std::string_view key, std::string* value, GetResult* result) const;

  std::unique_ptr<Iterator> NewIterator() const;

  const std::string& path() const { return path_; }
  const std::string& smallest_key() const { return smallest_key_; }
  const std::string& largest_key() const { return largest_key_; }
  uint64_t num_entries() const { return num_entries_; }
  uint64_t num_keys() const { return num_keys_; }  // distinct user keys
  uint64_t filter_bits() const { return filter_.empty() ? 0 : (filter_.size() - 1) * 8; }
  int filter_probes() const { return filter_.empty() ? 0 : static_cast<uint8_t>(filter_.back()); }
  uint64_t num_blocks() const { return index_.size(); }
  uint64_t file_size() const { return file_size_; }
  SequenceNumber min_seq() const { return min_seq_; }
  SequenceNumber max_seq() const { return max_seq_; }

  // Data blocks read from disk by this table so far (Get + iterators).
  uint64_t block_reads() const { return stats_.block_reads.load(std::memory_order_relaxed); }
  // Bloom statistics for Get(): filter consulted, filter said "no", and
  // filter said "maybe" but the key was not in the table (false positive).
  uint64_t bloom_checks() const { return stats_.bloom_checks.load(std::memory_order_relaxed); }
  uint64_t bloom_negatives() const {
    return stats_.bloom_negatives.load(std::memory_order_relaxed);
  }
  uint64_t bloom_false_positives() const {
    return stats_.bloom_false_positives.load(std::memory_order_relaxed);
  }

 private:
  friend class SSTableIterator;
  struct IndexEntry {
    std::string last_key;
    SequenceNumber last_seq;
    uint64_t offset;
    uint64_t size;  // block contents, without the 4-byte CRC
  };

  SSTableReader() = default;
  Status ReadBlock(uint64_t offset, uint64_t size, std::string* out) const;
  // Index of the first block whose last entry is >= (key, seq), or
  // index_.size() if none.
  size_t FindBlock(std::string_view key, SequenceNumber seq) const;

  std::string path_;
  int fd_ = -1;
  uint64_t file_size_ = 0;
  std::vector<IndexEntry> index_;
  std::string filter_;
  std::string smallest_key_, largest_key_;
  uint64_t num_entries_ = 0;
  uint64_t num_keys_ = 0;
  SequenceNumber min_seq_ = 0, max_seq_ = 0;
  bool use_bloom_ = true;
  void Count(std::atomic<uint64_t> ReadStats::*counter) const {
    (stats_.*counter).fetch_add(1, std::memory_order_relaxed);
    if (shared_stats_ != nullptr) (shared_stats_->*counter).fetch_add(1, std::memory_order_relaxed);
  }
  mutable ReadStats stats_;
  ReadStats* shared_stats_ = nullptr;
};

// Decodes one data-block entry at the front of *input and advances it.
bool DecodeBlockEntry(std::string_view* input, std::string_view* key, SequenceNumber* seq,
                      ValueType* type, std::string_view* value);

}  // namespace lsm
