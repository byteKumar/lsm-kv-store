#include "sstable.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include "coding.h"
#include "crc32.h"
#include "file_util.h"

namespace lsm {

namespace {
constexpr uint64_t kMagic = 0x31425453534d534cULL;  // bytes on disk spell "LSMSSTB1"
constexpr size_t kFooterSize = 7 * 8;               // 3 x (offset, size) + magic
constexpr size_t kCrcSize = 4;
}  // namespace

bool DecodeBlockEntry(std::string_view* input, std::string_view* key, SequenceNumber* seq,
                      ValueType* type, std::string_view* value) {
  if (!GetLengthPrefixed(input, key)) return false;
  if (input->size() < 9) return false;
  *seq = DecodeFixed64(input->data());
  uint8_t t = static_cast<uint8_t>((*input)[8]);
  if (t > static_cast<uint8_t>(ValueType::kValue)) return false;
  *type = static_cast<ValueType>(t);
  input->remove_prefix(9);
  return GetLengthPrefixed(input, value);
}

// ---------------------------------------------------------------- writer

SSTableWriter::~SSTableWriter() {
  if (fd_ >= 0) ::close(fd_);
}

Status SSTableWriter::Open(const std::string& path, const SSTableOptions& opts,
                           std::unique_ptr<SSTableWriter>* out) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return ErrnoStatus("open " + path);
  out->reset(new SSTableWriter(fd, opts));
  if (opts.bloom_bits_per_key > 0) {
    (*out)->bloom_ = std::make_unique<BloomFilterBuilder>(opts.bloom_bits_per_key);
  }
  return Status::OK();
}

Status SSTableWriter::Add(std::string_view key, SequenceNumber seq, ValueType type,
                          std::string_view value) {
  if (!status_.ok()) return status_;
  if (finished_) return Status::InvalidArgument("Add after Finish");
  if (num_entries_ > 0 && CompareInternal(key, seq, last_key_, last_seq_) <= 0) {
    return Status::InvalidArgument("keys added out of order");
  }
  if (num_entries_ == 0) smallest_key_.assign(key);
  // Several versions of a key are adjacent; the filter needs each key once.
  if (num_entries_ == 0 || key != std::string_view(last_key_)) {
    num_keys_++;
    if (bloom_) bloom_->AddKey(key);
  }
  PutLengthPrefixed(&block_, key);
  PutFixed64(&block_, seq);
  block_.push_back(static_cast<char>(type));
  PutLengthPrefixed(&block_, value);
  last_key_.assign(key);
  last_seq_ = seq;
  min_seq_ = std::min(min_seq_, seq);
  max_seq_ = std::max(max_seq_, seq);
  num_entries_++;
  // A block is closed once it reaches the target size, so blocks are a bit
  // over 4 KB, and one huge value gets a block to itself.
  if (block_.size() >= opts_.block_size) status_ = FlushBlock();
  return status_;
}

Status SSTableWriter::WriteBlockWithCrc(const std::string& contents, uint64_t* offset,
                                        uint64_t* size) {
  std::string crc;
  PutFixed32(&crc, crc32::Value(contents.data(), contents.size()));
  Status s = WriteAll(fd_, contents.data(), contents.size());
  if (s.ok()) s = WriteAll(fd_, crc.data(), crc.size());
  if (!s.ok()) return s;
  *offset = offset_;
  *size = contents.size();
  offset_ += contents.size() + kCrcSize;
  return Status::OK();
}

Status SSTableWriter::FlushBlock() {
  if (block_.empty()) return Status::OK();
  uint64_t off, size;
  Status s = WriteBlockWithCrc(block_, &off, &size);
  if (!s.ok()) return s;
  // The index stores the block's last key: every key in the block is <= it,
  // and every key in the next block is > it.
  PutLengthPrefixed(&index_, last_key_);
  PutFixed64(&index_, last_seq_);
  PutFixed64(&index_, off);
  PutFixed64(&index_, size);
  block_.clear();
  return Status::OK();
}

Status SSTableWriter::Finish(SyncMode mode) {
  if (!status_.ok()) return status_;
  if (finished_) return Status::InvalidArgument("Finish called twice");
  if (num_entries_ == 0) return Status::InvalidArgument("empty table");
  finished_ = true;

  Status s = FlushBlock();
  uint64_t index_off = 0, index_size = 0, filter_off = 0, filter_size = 0, props_off = 0,
           props_size = 0;
  if (s.ok()) s = WriteBlockWithCrc(index_, &index_off, &index_size);
  std::string filter = bloom_ ? bloom_->Finish() : std::string();
  if (s.ok()) s = WriteBlockWithCrc(filter, &filter_off, &filter_size);
  std::string props;
  PutLengthPrefixed(&props, smallest_key_);
  PutLengthPrefixed(&props, last_key_);
  PutFixed64(&props, num_entries_);
  PutFixed64(&props, min_seq_);
  PutFixed64(&props, max_seq_);
  PutFixed64(&props, num_keys_);
  if (s.ok()) s = WriteBlockWithCrc(props, &props_off, &props_size);

  std::string footer;
  for (uint64_t v : {index_off, index_size, filter_off, filter_size, props_off, props_size}) {
    PutFixed64(&footer, v);
  }
  PutFixed64(&footer, kMagic);
  if (s.ok()) s = WriteAll(fd_, footer.data(), footer.size());
  if (s.ok()) offset_ += footer.size();
  if (s.ok()) s = SyncFd(fd_, mode);
  if (::close(fd_) != 0 && s.ok()) s = ErrnoStatus("close");
  fd_ = -1;
  status_ = s;
  return s;
}

// ---------------------------------------------------------------- reader

SSTableReader::~SSTableReader() {
  if (fd_ >= 0) ::close(fd_);
}

Status SSTableReader::ReadBlock(uint64_t offset, uint64_t size, std::string* out) const {
  if (offset > file_size_ || size + kCrcSize > file_size_ - offset) {
    return Status::Corruption("block out of range in " + path_);
  }
  out->resize(size + kCrcSize);
  Status s = ReadAt(fd_, offset, size + kCrcSize, out->data());
  if (!s.ok()) return s;
  uint32_t stored = DecodeFixed32(out->data() + size);
  if (crc32::Value(out->data(), size) != stored) {
    return Status::Corruption("block checksum mismatch in " + path_ + " at offset " +
                              std::to_string(offset));
  }
  out->resize(size);
  return Status::OK();
}

Status SSTableReader::Open(const std::string& path, std::unique_ptr<SSTableReader>* out,
                           SSTableReadOptions opts) {
  std::unique_ptr<SSTableReader> t(new SSTableReader());
  t->path_ = path;
  t->use_bloom_ = opts.use_bloom;
  t->shared_stats_ = opts.shared_stats;
  t->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (t->fd_ < 0) return ErrnoStatus("open " + path);
  struct stat st;
  if (::fstat(t->fd_, &st) != 0) return ErrnoStatus("fstat " + path);
  t->file_size_ = static_cast<uint64_t>(st.st_size);
  if (t->file_size_ < kFooterSize) return Status::Corruption("file too short: " + path);

  char footer[kFooterSize];
  Status s = ReadAt(t->fd_, t->file_size_ - kFooterSize, kFooterSize, footer);
  if (!s.ok()) return s;
  if (DecodeFixed64(footer + 48) != kMagic) return Status::Corruption("bad magic: " + path);
  uint64_t f[6];
  for (int i = 0; i < 6; i++) f[i] = DecodeFixed64(footer + 8 * i);

  std::string index, props;
  s = t->ReadBlock(f[0], f[1], &index);
  if (s.ok()) s = t->ReadBlock(f[2], f[3], &t->filter_);
  if (s.ok()) s = t->ReadBlock(f[4], f[5], &props);
  if (!s.ok()) return s;

  std::string_view in(index);
  while (!in.empty()) {
    IndexEntry e;
    std::string_view k;
    if (!GetLengthPrefixed(&in, &k) || in.size() < 24) {
      return Status::Corruption("bad index block: " + path);
    }
    e.last_key.assign(k);
    e.last_seq = DecodeFixed64(in.data());
    e.offset = DecodeFixed64(in.data() + 8);
    e.size = DecodeFixed64(in.data() + 16);
    in.remove_prefix(24);
    t->index_.push_back(std::move(e));
  }

  std::string_view p(props), small, large;
  if (!GetLengthPrefixed(&p, &small) || !GetLengthPrefixed(&p, &large) || p.size() != 32) {
    return Status::Corruption("bad props block: " + path);
  }
  t->smallest_key_.assign(small);
  t->largest_key_.assign(large);
  t->num_entries_ = DecodeFixed64(p.data());
  t->min_seq_ = DecodeFixed64(p.data() + 8);
  t->max_seq_ = DecodeFixed64(p.data() + 16);
  t->num_keys_ = DecodeFixed64(p.data() + 24);
  if (t->index_.empty()) return Status::Corruption("table has no blocks: " + path);
  *out = std::move(t);
  return Status::OK();
}

size_t SSTableReader::FindBlock(std::string_view key, SequenceNumber seq) const {
  auto it = std::lower_bound(index_.begin(), index_.end(), 0,
                             [&](const IndexEntry& e, int) {
                               return CompareInternal(e.last_key, e.last_seq, key, seq) < 0;
                             });
  return static_cast<size_t>(it - index_.begin());
}

Status SSTableReader::Get(std::string_view key, std::string* value, GetResult* result) const {
  *result = GetResult::kNotFound;
  // Outside the table's key range: answer without touching the disk.
  if (key < std::string_view(smallest_key_) || key > std::string_view(largest_key_)) {
    return Status::OK();
  }
  // Bloom filter: a "no" is certain, so skip the disk read entirely.
  bool filter_used = use_bloom_ && !filter_.empty();
  if (filter_used) {
    Count(&ReadStats::bloom_checks);
    if (!BloomKeyMayMatch(filter_, key)) {
      Count(&ReadStats::bloom_negatives);
      return Status::OK();
    }
  }
  size_t b = FindBlock(key, kMaxSequenceNumber);
  if (b == index_.size()) return Status::OK();

  std::string block;
  Status s = ReadBlock(index_[b].offset, index_[b].size, &block);
  Count(&ReadStats::block_reads);
  if (!s.ok()) return s;

  std::string_view in(block), k, v;
  SequenceNumber seq;
  ValueType type;
  while (!in.empty()) {
    if (!DecodeBlockEntry(&in, &k, &seq, &type, &v)) {
      return Status::Corruption("bad data block in " + path_);
    }
    if (k < key) continue;
    if (k == key) {  // first match is the newest version
      if (type == ValueType::kDeletion) {
        *result = GetResult::kDeleted;
      } else {
        value->assign(v);
        *result = GetResult::kFound;
      }
    }
    break;
  }
  if (filter_used && *result == GetResult::kNotFound) {
    Count(&ReadStats::bloom_false_positives);
  }
  return Status::OK();
}

// ---------------------------------------------------------------- iterator

class SSTableIterator : public Iterator {
 public:
  explicit SSTableIterator(const SSTableReader* t) : t_(t) {}

  bool Valid() const override { return valid_; }
  Status status() const override { return status_; }
  std::string_view key() const override { return key_; }
  SequenceNumber seq() const override { return seq_; }
  ValueType type() const override { return type_; }
  std::string_view value() const override { return value_; }

  void SeekToFirst() override {
    valid_ = false;
    if (LoadBlock(0)) ParseNext();
  }

  void Seek(std::string_view target) override {
    valid_ = false;
    size_t b = t_->FindBlock(target, kMaxSequenceNumber);
    if (!LoadBlock(b)) return;
    // The chosen block's last entry is >= target, so this loop stops inside it.
    while (ParseNext() && key_ < target) {
    }
  }

  void Next() override { ParseNext(); }

 private:
  bool LoadBlock(size_t b) {
    block_idx_ = b;
    rest_ = std::string_view();
    if (b >= t_->index_.size()) return false;
    const auto& e = t_->index_[b];
    Status s = t_->ReadBlock(e.offset, e.size, &block_);
    t_->Count(&ReadStats::block_reads);
    if (!s.ok()) {
      status_ = s;
      return false;
    }
    rest_ = block_;
    return true;
  }

  // Moves to the next entry, crossing into the next block when needed.
  bool ParseNext() {
    valid_ = false;
    if (!status_.ok()) return false;
    while (rest_.empty()) {
      if (!LoadBlock(block_idx_ + 1)) return false;
    }
    if (!DecodeBlockEntry(&rest_, &key_, &seq_, &type_, &value_)) {
      status_ = Status::Corruption("bad data block in " + t_->path_);
      return false;
    }
    valid_ = true;
    return true;
  }

  const SSTableReader* t_;
  size_t block_idx_ = 0;
  std::string block_;
  std::string_view rest_;
  bool valid_ = false;
  Status status_;
  std::string_view key_, value_;
  SequenceNumber seq_ = 0;
  ValueType type_ = ValueType::kValue;
};

std::unique_ptr<Iterator> SSTableReader::NewIterator() const {
  return std::make_unique<SSTableIterator>(this);
}

}  // namespace lsm
