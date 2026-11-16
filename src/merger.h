#pragma once

// Iterators that combine several sources into one sorted view.
//
// MergingIterator: k-way merge of child iterators that are each sorted in
//   internal order (key asc, seq desc). Output is all their entries, every
//   version, in internal order.
//
// DBIterator: the user-facing view on top of a merge. For each key it shows
//   only the newest version visible at the snapshot sequence number, and
//   hides keys whose newest version is a tombstone.

#include <memory>
#include <string>
#include <vector>

#include "iterator.h"
#include "memtable.h"

namespace lsm {

// Adapts MemTable::Iterator to the common Iterator interface.
std::unique_ptr<Iterator> NewMemTableIterator(const MemTable* mem);

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children);

// `keep_alive` holds whatever the children point into (memtables, tables);
// it is released when the iterator is destroyed.
std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> merged, SequenceNumber snapshot,
                                        std::shared_ptr<const void> keep_alive);

}  // namespace lsm
