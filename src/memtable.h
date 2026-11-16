#pragma once

// In-memory write buffer. Every write (put or delete) becomes a new entry
// tagged with a sequence number; nothing is overwritten in place. Entries are
// ordered by key ascending, then sequence number descending, so the newest
// version of a key is always the first one a search lands on.

#include <atomic>
#include <string>
#include <string_view>

#include "skiplist.h"
#include "types.h"

namespace lsm {

class MemTable {
 public:
  enum class GetResult { kNotFound, kFound, kDeleted };

  MemTable() : list_(EntryCompare{}) {}

  // Not thread-safe with other Add() calls; safe with concurrent readers.
  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  // Looks up the newest version of key. kDeleted means a tombstone was found,
  // which callers must treat as "stop searching older data".
  GetResult Get(std::string_view key, std::string* value) const;

  // Rough bytes used, for deciding when to flush.
  size_t ApproximateMemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }
  size_t NumEntries() const { return num_entries_.load(std::memory_order_relaxed); }

  struct Entry {
    std::string key;
    SequenceNumber seq = 0;
    ValueType type = ValueType::kValue;
    std::string value;
  };

  struct EntryCompare {
    int operator()(const Entry& a, const Entry& b) const {
      int c = a.key.compare(b.key);
      if (c != 0) return c;
      if (a.seq > b.seq) return -1;  // newer first
      if (a.seq < b.seq) return 1;
      return 0;
    }
  };

  // Walks every entry (all versions) in internal order.
  class Iterator {
   public:
    explicit Iterator(const MemTable* mem) : it_(&mem->list_) {}
    bool Valid() const { return it_.Valid(); }
    void SeekToFirst() { it_.SeekToFirst(); }
    void Seek(std::string_view key);  // first entry with entry.key >= key
    void Next() { it_.Next(); }
    const Entry& entry() const { return it_.key(); }

   private:
    SkipList<Entry, EntryCompare>::Iterator it_;
  };

 private:
  SkipList<Entry, EntryCompare> list_;
  std::atomic<size_t> memory_usage_{0};
  std::atomic<size_t> num_entries_{0};
};

}  // namespace lsm
