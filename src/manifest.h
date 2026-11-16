#pragma once

// File names inside a database directory, and the MANIFEST that records
// which SSTables are live.
//
//   <dir>/LOCK          held with flock() while a process has the DB open
//   <dir>/MANIFEST      current state (rewritten atomically, see below)
//   <dir>/000007.log    write-ahead log for one memtable
//   <dir>/000008.sst    an SSTable
//
// The MANIFEST is replaced atomically: write MANIFEST.tmp, sync it, rename()
// it over MANIFEST, then sync the directory. rename() within a directory is
// atomic, so after a crash we see either the old or the new manifest, never
// half of one. Files that exist on disk but are not listed (a table that was
// being written when we crashed, a log that is no longer needed) are deleted
// on the next open.

#include <cstdint>
#include <string>
#include <vector>

#include "status.h"
#include "types.h"
#include "wal.h"  // SyncMode

namespace lsm {

enum class FileType { kLog, kTable, kOther };

std::string LogFileName(const std::string& dir, uint64_t number);
std::string TableFileName(const std::string& dir, uint64_t number);
std::string ManifestFileName(const std::string& dir);
std::string LockFileName(const std::string& dir);

// Parses "000123.log" / "000123.sst". Returns kOther for anything else.
FileType ParseFileName(const std::string& name, uint64_t* number);

struct ManifestState {
  uint64_t next_file_number = 1;
  uint64_t log_number = 0;        // logs with a smaller number are already in tables
  SequenceNumber last_seq = 0;    // highest sequence number known to be in tables
  std::vector<uint64_t> tables;   // live tables, oldest first
};

// Writes state atomically. *bytes_written gets the manifest size.
Status WriteManifest(const std::string& dir, const ManifestState& state, SyncMode mode,
                     uint64_t* bytes_written);

// Reads the manifest. NotFound if there is none (a new database).
Status ReadManifest(const std::string& dir, ManifestState* state);

}  // namespace lsm
