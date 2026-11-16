#pragma once

#include <cstdint>

namespace lsm {

// Every write carries a type. A deletion is stored as a "tombstone" entry so
// that it can hide older values of the same key in older tables.
enum class ValueType : uint8_t {
  kDeletion = 0,
  kValue = 1,
};

using SequenceNumber = uint64_t;
constexpr SequenceNumber kMaxSequenceNumber = UINT64_MAX;

}  // namespace lsm
