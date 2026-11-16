#include "memtable.h"

namespace lsm {

namespace {
// Per-entry overhead estimate: the Entry struct plus the skiplist node and an
// average of 1.33 forward pointers (branching factor 4).
constexpr size_t kEntryOverhead = sizeof(MemTable::Entry) + 48;
}  // namespace

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value) {
  Entry e;
  e.key.assign(key);
  e.seq = seq;
  e.type = type;
  e.value.assign(value);
  list_.Insert(e);
  memory_usage_.fetch_add(key.size() + value.size() + kEntryOverhead, std::memory_order_relaxed);
  num_entries_.fetch_add(1, std::memory_order_relaxed);
}

MemTable::GetResult MemTable::Get(std::string_view key, std::string* value) const {
  // Seeking to (key, max seq) lands on the newest version of key, if any.
  Entry target;
  target.key.assign(key);
  target.seq = kMaxSequenceNumber;
  SkipList<Entry, EntryCompare>::Iterator it(&list_);
  it.Seek(target);
  if (!it.Valid() || it.key().key != key) return GetResult::kNotFound;
  if (it.key().type == ValueType::kDeletion) return GetResult::kDeleted;
  value->assign(it.key().value);
  return GetResult::kFound;
}

void MemTable::Iterator::Seek(std::string_view key) {
  Entry target;
  target.key.assign(key);
  target.seq = kMaxSequenceNumber;
  it_.Seek(target);
}

}  // namespace lsm
