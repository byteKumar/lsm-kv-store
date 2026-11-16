#pragma once

// Common interface for walking sorted entries (memtable, SSTable, and later
// the merging iterator). Entries come out in internal order: key ascending,
// then sequence number descending (newest version of a key first).

#include <string_view>

#include "status.h"
#include "types.h"

namespace lsm {

class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  // Positions at the first entry whose key is >= key (newest version first).
  virtual void Seek(std::string_view key) = 0;
  virtual void Next() = 0;
  // The views below stay valid until the next call that moves the iterator.
  virtual std::string_view key() const = 0;
  virtual SequenceNumber seq() const = 0;
  virtual ValueType type() const = 0;
  virtual std::string_view value() const = 0;
  // Non-OK if reading hit an I/O error or corruption; the iterator is then
  // no longer Valid().
  virtual Status status() const = 0;
};

// Internal ordering used everywhere: key ascending, then seq descending.
inline int CompareInternal(std::string_view ak, SequenceNumber as, std::string_view bk,
                           SequenceNumber bs) {
  int c = ak.compare(bk);
  if (c != 0) return c < 0 ? -1 : 1;
  if (as > bs) return -1;
  if (as < bs) return 1;
  return 0;
}

}  // namespace lsm
