#include "wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "coding.h"
#include "crc32.h"
#include "file_util.h"
#include "memtable.h"

namespace lsm {

namespace {
constexpr size_t kHeaderSize = 8;               // crc32 + length
constexpr uint32_t kMaxRecordSize = 64u << 20;  // sanity limit for a length field
}  // namespace

const char* SyncModeName(SyncMode m) {
  switch (m) {
    case SyncMode::kNone: return "none";
    case SyncMode::kFsync: return "fsync";
    case SyncMode::kFullFsync: return "full";
  }
  return "?";
}

bool ParseSyncMode(std::string_view s, SyncMode* out) {
  if (s == "none") *out = SyncMode::kNone;
  else if (s == "fsync") *out = SyncMode::kFsync;
  else if (s == "full") *out = SyncMode::kFullFsync;
  else return false;
  return true;
}

WalWriter::~WalWriter() { Close(); }

Status WalWriter::Open(const std::string& path, SyncMode mode, uint64_t valid_bytes,
                       std::unique_ptr<WalWriter>* out) {
  struct stat st;
  bool existed = ::stat(path.c_str(), &st) == 0;
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return ErrnoStatus("open " + path);
  if (::ftruncate(fd, static_cast<off_t>(valid_bytes)) != 0 ||
      ::lseek(fd, static_cast<off_t>(valid_bytes), SEEK_SET) < 0) {
    Status s = ErrnoStatus("truncate " + path);
    ::close(fd);
    return s;
  }
  Status s = SyncFd(fd, mode);
  // A new file's directory entry is only durable once the directory itself
  // is synced; otherwise a power loss could lose the whole file.
  if (s.ok() && !existed) s = SyncParentDir(path, mode);
  if (!s.ok()) {
    ::close(fd);
    return s;
  }
  out->reset(new WalWriter(fd, mode, valid_bytes));
  return Status::OK();
}

Status WalWriter::AddRecord(std::string_view payload) {
  if (fd_ < 0) return Status::IOError("log is closed");
  if (payload.size() > kMaxRecordSize) return Status::InvalidArgument("record too large");
  // Build header + payload in one buffer so it goes out in a single write(2).
  std::string buf;
  buf.reserve(kHeaderSize + payload.size());
  buf.append(4, '\0');  // CRC placeholder
  PutFixed32(&buf, static_cast<uint32_t>(payload.size()));
  buf.append(payload.data(), payload.size());
  uint32_t crc = crc32::Value(buf.data() + 4, buf.size() - 4);
  for (int i = 0; i < 4; i++) buf[i] = static_cast<char>((crc >> (8 * i)) & 0xff);

  Status s = WriteAll(fd_, buf.data(), buf.size());
  if (!s.ok()) return s;
  bytes_written_ += buf.size();
  file_size_ += buf.size();
  return SyncFd(fd_, mode_);
}

Status WalWriter::Sync() {
  if (fd_ < 0) return Status::OK();
  return SyncFd(fd_, mode_ == SyncMode::kNone ? SyncMode::kFsync : mode_);
}

Status WalWriter::Close() {
  if (fd_ < 0) return Status::OK();
  int r = ::close(fd_);
  fd_ = -1;
  return r == 0 ? Status::OK() : ErrnoStatus("close");
}

Status ReadWal(const std::string& path,
               const std::function<Status(std::string_view payload)>& fn,
               WalRecoveryResult* result) {
  *result = WalRecoveryResult{};
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return Status::OK();
    return ErrnoStatus("open " + path);
  }
  // A log only ever holds one memtable's worth of writes, so reading it
  // whole keeps the parsing simple.
  std::vector<char> data;
  char chunk[1 << 16];
  while (true) {
    ssize_t r = ::read(fd, chunk, sizeof(chunk));
    if (r < 0) {
      if (errno == EINTR) continue;
      Status s = ErrnoStatus("read " + path);
      ::close(fd);
      return s;
    }
    if (r == 0) break;
    data.insert(data.end(), chunk, chunk + r);
  }
  ::close(fd);
  result->file_bytes = data.size();

  size_t pos = 0;
  while (pos < data.size()) {
    size_t left = data.size() - pos;
    if (left < kHeaderSize) {
      result->stop_reason = "partial header at offset " + std::to_string(pos);
      break;
    }
    uint32_t stored_crc = DecodeFixed32(&data[pos]);
    uint32_t len = DecodeFixed32(&data[pos + 4]);
    if (len > kMaxRecordSize || left - kHeaderSize < len) {
      result->stop_reason = "partial record at offset " + std::to_string(pos);
      break;
    }
    uint32_t actual_crc = crc32::Value(&data[pos + 4], 4 + len);
    if (actual_crc != stored_crc) {
      result->stop_reason = "checksum mismatch at offset " + std::to_string(pos);
      break;
    }
    Status s = fn(std::string_view(&data[pos + kHeaderSize], len));
    if (!s.ok()) return s;
    pos += kHeaderSize + len;
    result->records++;
  }
  result->valid_bytes = pos;
  result->tail_dropped = pos < data.size();
  return Status::OK();
}

std::string EncodeWalEntry(SequenceNumber seq, ValueType type, std::string_view key,
                           std::string_view value) {
  std::string out;
  out.reserve(8 + 1 + 4 + key.size() + 4 + value.size());
  PutFixed64(&out, seq);
  out.push_back(static_cast<char>(type));
  PutLengthPrefixed(&out, key);
  PutLengthPrefixed(&out, value);
  return out;
}

bool DecodeWalEntry(std::string_view payload, SequenceNumber* seq, ValueType* type,
                    std::string_view* key, std::string_view* value) {
  if (payload.size() < 9) return false;
  *seq = DecodeFixed64(payload.data());
  uint8_t t = static_cast<uint8_t>(payload[8]);
  if (t > static_cast<uint8_t>(ValueType::kValue)) return false;
  *type = static_cast<ValueType>(t);
  payload.remove_prefix(9);
  return GetLengthPrefixed(&payload, key) && GetLengthPrefixed(&payload, value) &&
         payload.empty();
}

Status RecoverMemTable(const std::string& path, MemTable* mem, SequenceNumber* max_seq,
                       WalRecoveryResult* result) {
  return ReadWal(
      path,
      [&](std::string_view payload) {
        SequenceNumber seq;
        ValueType type;
        std::string_view key, value;
        // The CRC matched, so a payload that does not parse is a bug or real
        // corruption, not a torn write. Refuse to guess.
        if (!DecodeWalEntry(payload, &seq, &type, &key, &value)) {
          return Status::Corruption("bad WAL entry in " + path);
        }
        mem->Add(seq, type, key, value);
        if (seq > *max_seq) *max_seq = seq;
        return Status::OK();
      },
      result);
}

}  // namespace lsm
