#include "manifest.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "coding.h"
#include "crc32.h"
#include "file_util.h"

namespace lsm {

namespace {
constexpr uint64_t kManifestMagic = 0x31464e4d4d534cULL;  // "LSMMNF1"

std::string NumberedName(const std::string& dir, uint64_t number, const char* ext) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(number), ext);
  return dir + buf;
}
}  // namespace

std::string LogFileName(const std::string& dir, uint64_t number) {
  return NumberedName(dir, number, "log");
}
std::string TableFileName(const std::string& dir, uint64_t number) {
  return NumberedName(dir, number, "sst");
}
std::string ManifestFileName(const std::string& dir) { return dir + "/MANIFEST"; }
std::string LockFileName(const std::string& dir) { return dir + "/LOCK"; }

FileType ParseFileName(const std::string& name, uint64_t* number) {
  size_t dot = name.find('.');
  if (dot == std::string::npos || dot == 0) return FileType::kOther;
  uint64_t n = 0;
  for (size_t i = 0; i < dot; i++) {
    if (name[i] < '0' || name[i] > '9') return FileType::kOther;
    n = n * 10 + static_cast<uint64_t>(name[i] - '0');
  }
  std::string ext = name.substr(dot + 1);
  *number = n;
  if (ext == "log") return FileType::kLog;
  if (ext == "sst") return FileType::kTable;
  return FileType::kOther;
}

Status WriteManifest(const std::string& dir, const ManifestState& state, SyncMode mode,
                     uint64_t* bytes_written) {
  std::string payload;
  PutFixed64(&payload, kManifestMagic);
  PutFixed64(&payload, state.next_file_number);
  PutFixed64(&payload, state.log_number);
  PutFixed64(&payload, state.last_seq);
  PutFixed64(&payload, state.tables.size());
  for (uint64_t t : state.tables) PutFixed64(&payload, t);
  std::string data;
  PutFixed32(&data, crc32::Value(payload.data(), payload.size()));
  PutFixed32(&data, static_cast<uint32_t>(payload.size()));
  data += payload;

  std::string final_name = ManifestFileName(dir);
  std::string tmp = final_name + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return ErrnoStatus("open " + tmp);
  Status s = WriteAll(fd, data.data(), data.size());
  if (s.ok()) s = SyncFd(fd, mode);
  if (::close(fd) != 0 && s.ok()) s = ErrnoStatus("close " + tmp);
  if (s.ok() && ::rename(tmp.c_str(), final_name.c_str()) != 0) s = ErrnoStatus("rename manifest");
  if (s.ok()) s = SyncParentDir(final_name, mode);
  if (s.ok() && bytes_written != nullptr) *bytes_written = data.size();
  return s;
}

Status ReadManifest(const std::string& dir, ManifestState* state) {
  std::string name = ManifestFileName(dir);
  std::ifstream in(name, std::ios::binary);
  if (!in) {
    if (errno == ENOENT) return Status::NotFound("no manifest");
    return ErrnoStatus("open " + name);
  }
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() < 8) return Status::Corruption("manifest too short");
  uint32_t crc = DecodeFixed32(data.data());
  uint32_t len = DecodeFixed32(data.data() + 4);
  if (len != data.size() - 8) return Status::Corruption("manifest length mismatch");
  if (crc32::Value(data.data() + 8, len) != crc) return Status::Corruption("manifest checksum");
  const char* p = data.data() + 8;
  if (len < 40 || DecodeFixed64(p) != kManifestMagic) return Status::Corruption("manifest magic");
  state->next_file_number = DecodeFixed64(p + 8);
  state->log_number = DecodeFixed64(p + 16);
  state->last_seq = DecodeFixed64(p + 24);
  uint64_t n = DecodeFixed64(p + 32);
  if (n > (len - 40) / 8 || len != 40 + 8 * n) return Status::Corruption("manifest table count");
  state->tables.clear();
  for (uint64_t i = 0; i < n; i++) state->tables.push_back(DecodeFixed64(p + 40 + 8 * i));
  return Status::OK();
}

}  // namespace lsm
