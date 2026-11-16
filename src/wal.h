#pragma once

// Write-ahead log. Every write is appended here before it goes into the
// memtable, so after a crash the memtable can be rebuilt by replaying the log.
//
// On-disk record format (little-endian):
//
//   +-----------+------------+-------------------+
//   | crc32 (4) | length (4) | payload (length)  |
//   +-----------+------------+-------------------+
//
// The CRC covers the length field and the payload, so a damaged length is
// caught too. A crash in the middle of an append leaves a "torn" record at
// the end of the file: a short header, a short payload, or a CRC mismatch.
// Recovery stops at the first such record and keeps everything before it.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "status.h"
#include "types.h"

namespace lsm {

enum class SyncMode {
  kNone,       // write(2) only: survives a process crash, not a power loss
  kFsync,      // fsync(2) after each record (macOS: may stay in the drive cache)
  kFullFsync,  // macOS F_FULLFSYNC: asks the drive to flush its cache too
};

const char* SyncModeName(SyncMode m);
bool ParseSyncMode(std::string_view s, SyncMode* out);

class WalWriter {
 public:
  ~WalWriter();

  // Opens (or creates) the log at path and positions writes at offset
  // valid_bytes, cutting off anything after it (for example a torn record
  // found during recovery). Without that cut, new records would sit behind
  // garbage and the next recovery would stop before reaching them.
  static Status Open(const std::string& path, SyncMode mode, uint64_t valid_bytes,
                     std::unique_ptr<WalWriter>* out);

  // Appends one record. When this returns OK the record has been handed to
  // the OS (and synced, if the mode says so).
  Status AddRecord(std::string_view payload);
  Status Sync();
  Status Close();

  uint64_t BytesWritten() const { return bytes_written_; }  // this session only
  uint64_t FileSize() const { return file_size_; }

 private:
  WalWriter(int fd, SyncMode mode, uint64_t size) : fd_(fd), mode_(mode), file_size_(size) {}
  int fd_;
  SyncMode mode_;
  uint64_t file_size_;
  uint64_t bytes_written_ = 0;
};

struct WalRecoveryResult {
  uint64_t records = 0;      // good records replayed
  uint64_t valid_bytes = 0;  // file offset just past the last good record
  uint64_t file_bytes = 0;   // file size found on disk
  bool tail_dropped = false;  // true if bytes after valid_bytes were ignored
  std::string stop_reason;    // why reading stopped early, if it did
};

// Reads every good record in order and calls fn(payload) for each one. A
// missing file is not an error (0 records). Returns an error only for I/O
// failures or if fn returns a non-OK status.
Status ReadWal(const std::string& path,
               const std::function<Status(std::string_view payload)>& fn,
               WalRecoveryResult* result);

// Payload encoding for one key-value operation.
std::string EncodeWalEntry(SequenceNumber seq, ValueType type, std::string_view key,
                           std::string_view value);
bool DecodeWalEntry(std::string_view payload, SequenceNumber* seq, ValueType* type,
                    std::string_view* key, std::string_view* value);

class MemTable;
// Replays the log at path into mem. *max_seq is raised to the largest
// sequence number seen.
Status RecoverMemTable(const std::string& path, MemTable* mem, SequenceNumber* max_seq,
                       WalRecoveryResult* result);

}  // namespace lsm
