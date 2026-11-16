#pragma once

// Small POSIX helpers shared by the WAL and SSTable code.

#include <cstddef>
#include <cstdint>
#include <string>

#include "status.h"
#include "wal.h"  // SyncMode

namespace lsm {

Status ErrnoStatus(const std::string& what);

// Loops over short writes and EINTR.
Status WriteAll(int fd, const char* data, size_t n);

// Reads exactly n bytes at offset (pread loops over short reads). Fails if
// the file ends first.
Status ReadAt(int fd, uint64_t offset, size_t n, char* out);

// fsync / F_FULLFSYNC according to mode; no-op for kNone.
Status SyncFd(int fd, SyncMode mode);

// Syncs the directory holding path, so a newly created or renamed file's
// directory entry is durable too.
Status SyncParentDir(const std::string& path, SyncMode mode);

std::string DirName(const std::string& path);

}  // namespace lsm
