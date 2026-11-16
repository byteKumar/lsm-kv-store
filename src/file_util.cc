#include "file_util.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace lsm {

Status ErrnoStatus(const std::string& what) {
  return Status::IOError(what + ": " + std::strerror(errno));
}

Status WriteAll(int fd, const char* data, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, data, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus("write");
    }
    data += w;
    n -= static_cast<size_t>(w);
  }
  return Status::OK();
}

Status ReadAt(int fd, uint64_t offset, size_t n, char* out) {
  while (n > 0) {
    ssize_t r = ::pread(fd, out, n, static_cast<off_t>(offset));
    if (r < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus("pread");
    }
    if (r == 0) return Status::Corruption("unexpected end of file");
    out += r;
    offset += static_cast<uint64_t>(r);
    n -= static_cast<size_t>(r);
  }
  return Status::OK();
}

Status SyncFd(int fd, SyncMode mode) {
  if (mode == SyncMode::kNone) return Status::OK();
#ifdef F_FULLFSYNC
  if (mode == SyncMode::kFullFsync) {
    if (::fcntl(fd, F_FULLFSYNC) == 0) return Status::OK();
    // Some file systems do not support F_FULLFSYNC; fall back to fsync.
  }
#endif
  if (::fsync(fd) != 0) return ErrnoStatus("fsync");
  return Status::OK();
}

std::string DirName(const std::string& path) {
  size_t pos = path.rfind('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

Status SyncParentDir(const std::string& path, SyncMode mode) {
  if (mode == SyncMode::kNone) return Status::OK();
  int dfd = ::open(DirName(path).c_str(), O_RDONLY | O_CLOEXEC);
  if (dfd < 0) return ErrnoStatus("open dir");
  Status s = SyncFd(dfd, mode);
  ::close(dfd);
  return s;
}

}  // namespace lsm
