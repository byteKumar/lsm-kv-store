#include "db.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <set>

#include "file_util.h"
#include "merger.h"

namespace lsm {

struct DB::TableHandle {
  uint64_t number = 0;
  std::unique_ptr<SSTableReader> reader;
};

// An immutable snapshot of what a reader needs. Replaced (never modified)
// under mu_, so a reader holding a shared_ptr to one sees a consistent set
// even while flushes and compactions install new versions.
struct DB::Version {
  std::shared_ptr<MemTable> mem;
  std::shared_ptr<MemTable> imm;  // frozen memtable being flushed, or null
  std::vector<std::shared_ptr<TableHandle>> tables;  // oldest first
};

DB::DB(const Options& options, std::string dir) : options_(options), dir_(std::move(dir)) {}

DB::~DB() {
  {
    std::lock_guard<std::mutex> l(mu_);
    shutting_down_ = true;
  }
  bg_cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();
  if (wal_) wal_->Close();
  if (lock_fd_ >= 0) ::close(lock_fd_);  // also releases the flock
}

Status DB::Open(const Options& options, const std::string& dir, std::unique_ptr<DB>* out) {
  std::unique_ptr<DB> db(new DB(options, dir));
  Status s = db->Recover();
  if (!s.ok()) return s;
  db->bg_thread_ = std::thread([p = db.get()] { p->BackgroundThread(); });
  *out = std::move(db);
  return Status::OK();
}

std::shared_ptr<const DB::Version> DB::CurrentVersion() const {
  std::lock_guard<std::mutex> l(mu_);
  return version_;
}

Status DB::OpenTable(uint64_t number, std::shared_ptr<TableHandle>* handle) {
  auto h = std::make_shared<TableHandle>();
  h->number = number;
  SSTableReadOptions ro;
  ro.use_bloom = options_.use_bloom;
  ro.shared_stats = &read_stats_;
  Status s = SSTableReader::Open(TableFileName(dir_, number), &h->reader, ro);
  if (!s.ok()) return s;
  *handle = std::move(h);
  return Status::OK();
}

Status DB::WriteTable(Iterator* it, uint64_t number, uint64_t* file_size,
                      std::shared_ptr<TableHandle>* handle) {
  SSTableOptions opts;
  opts.block_size = options_.block_size;
  opts.bloom_bits_per_key = options_.bloom_bits_per_key;
  std::unique_ptr<SSTableWriter> w;
  std::string path = TableFileName(dir_, number);
  Status s = SSTableWriter::Open(path, opts, &w);
  if (!s.ok()) return s;
  // Keep only the newest version of each key: nothing (no snapshot) can read
  // the older ones. Tombstones are kept because older tables may still hold
  // the key and the tombstone has to hide it.
  std::string prev;
  bool has_prev = false;
  for (it->SeekToFirst(); it->Valid() && s.ok(); it->Next()) {
    if (has_prev && it->key() == std::string_view(prev)) continue;
    prev.assign(it->key());
    has_prev = true;
    s = w->Add(it->key(), it->seq(), it->type(), it->value());
  }
  if (s.ok()) s = it->status();
  if (s.ok()) s = w->Finish(options_.sync_mode);
  if (!s.ok()) {
    w.reset();
    ::unlink(path.c_str());
    return s;
  }
  *file_size = w->FileSize();
  return OpenTable(number, handle);
}

ManifestState DB::MakeManifestState(const std::vector<std::shared_ptr<TableHandle>>& tables,
                                    uint64_t log_number) const {
  ManifestState st;
  {
    std::lock_guard<std::mutex> l(mu_);
    st.next_file_number = next_file_number_;
  }
  st.log_number = log_number;
  st.last_seq = last_seq_.load();
  for (const auto& t : tables) st.tables.push_back(t->number);
  return st;
}

Status DB::Recover() {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) return Status::IOError("create " + dir_ + ": " + ec.message());

  // Only one process may have the database open; a second one would append
  // to the same WAL and corrupt it. The lock disappears when the process
  // dies, including after kill -9.
  lock_fd_ = ::open(LockFileName(dir_).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd_ < 0) return ErrnoStatus("open LOCK");
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    return Status::IOError("database " + dir_ + " is locked by another process");
  }

  ManifestState st;
  Status s = ReadManifest(dir_, &st);
  if (s.IsNotFound()) {
    st = ManifestState{};
  } else if (!s.ok()) {
    return s;
  }

  std::vector<std::shared_ptr<TableHandle>> tables;
  SequenceNumber max_seq = st.last_seq;
  for (uint64_t n : st.tables) {
    std::shared_ptr<TableHandle> h;
    s = OpenTable(n, &h);
    if (!s.ok()) return s;  // a listed table that is missing or damaged is fatal
    max_seq = std::max(max_seq, h->reader->max_seq());
    tables.push_back(std::move(h));
  }

  // Find WAL files that are not yet covered by a table.
  std::vector<uint64_t> logs;
  uint64_t max_number = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
    uint64_t n;
    FileType t = ParseFileName(entry.path().filename().string(), &n);
    if (t == FileType::kOther) continue;
    max_number = std::max(max_number, n);
    if (t == FileType::kLog && n >= st.log_number) logs.push_back(n);
  }
  if (ec) return Status::IOError("list " + dir_ + ": " + ec.message());
  std::sort(logs.begin(), logs.end());
  next_file_number_ = std::max(st.next_file_number, max_number + 1);

  auto recovered = std::make_shared<MemTable>();
  for (uint64_t n : logs) {
    WalRecoveryResult r;
    s = RecoverMemTable(LogFileName(dir_, n), recovered.get(), &max_seq, &r);
    if (!s.ok()) return s;
  }
  last_seq_.store(max_seq);
  visible_seq_.store(max_seq);

  // Put replayed writes into a table so the old logs can be dropped.
  if (recovered->NumEntries() > 0) {
    auto it = NewMemTableIterator(recovered.get());
    uint64_t size = 0;
    std::shared_ptr<TableHandle> h;
    s = WriteTable(it.get(), next_file_number_++, &size, &h);
    if (!s.ok()) return s;
    flush_bytes_ += size;
    flushes_++;
    tables.push_back(std::move(h));
  }

  log_number_ = next_file_number_++;
  s = WalWriter::Open(LogFileName(dir_, log_number_), options_.sync_mode, 0, &wal_);
  if (!s.ok()) return s;
  uint64_t mbytes = 0;
  s = WriteManifest(dir_, MakeManifestState(tables, log_number_), options_.sync_mode, &mbytes);
  if (!s.ok()) return s;
  manifest_bytes_ += mbytes;

  auto v = std::make_shared<Version>();
  v->mem = std::make_shared<MemTable>();
  v->tables = std::move(tables);
  version_ = std::move(v);
  DeleteObsoleteFiles();
  return Status::OK();
}

void DB::DeleteObsoleteFiles() {
  std::set<uint64_t> live_tables;
  uint64_t live_log, imm_log;
  {
    std::lock_guard<std::mutex> l(mu_);
    for (const auto& t : version_->tables) live_tables.insert(t->number);
    live_log = log_number_;
    imm_log = imm_log_number_;
  }
  std::error_code ec;
  std::vector<std::filesystem::path> doomed;
  for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
    std::string name = entry.path().filename().string();
    uint64_t n;
    FileType t = ParseFileName(name, &n);
    bool remove = (t == FileType::kLog && n != live_log && n != imm_log) ||
                  (t == FileType::kTable && live_tables.count(n) == 0) ||
                  name == "MANIFEST.tmp";
    if (remove) doomed.push_back(entry.path());
  }
  for (const auto& p : doomed) std::filesystem::remove(p, ec);
}

Status DB::Put(std::string_view key, std::string_view value) {
  return Write(ValueType::kValue, key, value);
}

Status DB::Delete(std::string_view key) { return Write(ValueType::kDeletion, key, ""); }

Status DB::Write(ValueType type, std::string_view key, std::string_view value) {
  std::unique_lock<std::mutex> wl(write_mu_);
  {
    std::lock_guard<std::mutex> l(mu_);
    if (!bg_error_.ok()) return bg_error_;
  }
  if (CurrentVersion()->mem->ApproximateMemoryUsage() >= options_.memtable_size) {
    Status s = SwitchMemTable(&wl);
    if (!s.ok()) return s;
  }
  SequenceNumber seq = last_seq_.load() + 1;
  uint64_t before = wal_->FileSize();
  Status s = wal_->AddRecord(EncodeWalEntry(seq, type, key, value));
  if (!s.ok()) {
    // A failed append may have left a partial record in the log. Anything
    // appended after it would be unreachable on recovery, so stop writing.
    std::lock_guard<std::mutex> l(mu_);
    bg_error_ = s;
    return s;
  }
  wal_bytes_ += wal_->FileSize() - before;
  CurrentVersion()->mem->Add(seq, type, key, value);
  last_seq_.store(seq);
  visible_seq_.store(seq, std::memory_order_release);
  user_bytes_ += key.size() + value.size();
  return Status::OK();
}

Status DB::SwitchMemTable(std::unique_lock<std::mutex>* /*write_lock*/) {
  std::unique_lock<std::mutex> l(mu_);
  // Only one frozen memtable at a time: if the last one is still being
  // flushed, this writer (and every writer behind it) waits. This is the
  // write stall that keeps memory bounded when disk is slower than writes.
  if (version_->imm && bg_error_.ok()) write_stalls_++;
  while (version_->imm && bg_error_.ok()) done_cv_.wait(l);
  if (!bg_error_.ok()) return bg_error_;
  uint64_t new_log = next_file_number_++;
  l.unlock();

  std::unique_ptr<WalWriter> w;
  Status s = WalWriter::Open(LogFileName(dir_, new_log), options_.sync_mode, 0, &w);
  if (!s.ok()) return s;

  l.lock();
  auto v = std::make_shared<Version>(*version_);
  v->imm = v->mem;
  v->mem = std::make_shared<MemTable>();
  version_ = std::move(v);
  imm_log_number_ = log_number_;
  log_number_ = new_log;
  l.unlock();

  wal_->Close();
  wal_ = std::move(w);
  bg_cv_.notify_one();
  return Status::OK();
}

void DB::BackgroundThread() {
  std::unique_lock<std::mutex> l(mu_);
  while (true) {
    bool flush = version_->imm != nullptr && bg_error_.ok();
    if (flush) {
      l.unlock();
      Status s = FlushImmutable();
      l.lock();
      if (!s.ok()) bg_error_ = s;
      done_cv_.notify_all();
      continue;
    }
    if (shutting_down_) break;  // pending flushes are finished first
    bg_cv_.wait(l);
  }
}

Status DB::FlushImmutable() {
  std::shared_ptr<const Version> v;
  uint64_t number, log_number, old_log;
  {
    std::lock_guard<std::mutex> l(mu_);
    v = version_;
    number = next_file_number_++;
    log_number = log_number_;
    old_log = imm_log_number_;
  }
  auto it = NewMemTableIterator(v->imm.get());
  uint64_t size = 0;
  std::shared_ptr<TableHandle> h;
  Status s = WriteTable(it.get(), number, &size, &h);
  if (!s.ok()) return s;
  flush_bytes_ += size;
  flushes_++;

  // Only this thread changes the table list, so v->tables is still current.
  std::vector<std::shared_ptr<TableHandle>> tables = v->tables;
  tables.push_back(h);
  // The manifest now says: everything in logs older than log_number is in a
  // table. Only after it is durable may the frozen memtable's log go away.
  uint64_t mbytes = 0;
  s = WriteManifest(dir_, MakeManifestState(tables, log_number), options_.sync_mode, &mbytes);
  if (!s.ok()) return s;
  manifest_bytes_ += mbytes;
  {
    std::lock_guard<std::mutex> l(mu_);
    auto nv = std::make_shared<Version>(*version_);
    nv->imm = nullptr;
    nv->tables = std::move(tables);
    version_ = std::move(nv);
    imm_log_number_ = 0;
  }
  ::unlink(LogFileName(dir_, old_log).c_str());
  return Status::OK();
}

Status DB::Flush() {
  std::unique_lock<std::mutex> wl(write_mu_);
  if (CurrentVersion()->mem->NumEntries() > 0) {
    Status s = SwitchMemTable(&wl);
    if (!s.ok()) return s;
  }
  std::unique_lock<std::mutex> l(mu_);
  while (version_->imm && bg_error_.ok()) done_cv_.wait(l);
  return bg_error_;
}

Status DB::Get(std::string_view key, std::string* value) {
  std::shared_ptr<const Version> v = CurrentVersion();
  for (const MemTable* m : {v->mem.get(), v->imm.get()}) {
    if (m == nullptr) continue;
    switch (m->Get(key, value)) {
      case MemTable::GetResult::kFound: return Status::OK();
      case MemTable::GetResult::kDeleted: return Status::NotFound();
      case MemTable::GetResult::kNotFound: break;
    }
  }
  for (auto t = v->tables.rbegin(); t != v->tables.rend(); ++t) {
    SSTableReader::GetResult r;
    Status s = (*t)->reader->Get(key, value, &r);
    if (!s.ok()) return s;
    if (r == SSTableReader::GetResult::kFound) return Status::OK();
    if (r == SSTableReader::GetResult::kDeleted) return Status::NotFound();
  }
  return Status::NotFound();
}

std::unique_ptr<Iterator> DB::NewIterator() {
  // Read the snapshot sequence before the version: every write up to it is
  // then guaranteed to be in this version's memtables or tables.
  SequenceNumber snapshot = visible_seq_.load(std::memory_order_acquire);
  std::shared_ptr<const Version> v = CurrentVersion();
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(NewMemTableIterator(v->mem.get()));
  if (v->imm) children.push_back(NewMemTableIterator(v->imm.get()));
  for (const auto& t : v->tables) children.push_back(t->reader->NewIterator());
  return NewDBIterator(NewMergingIterator(std::move(children)), snapshot, v);
}

Status DB::Scan(std::string_view start, size_t limit,
                std::vector<std::pair<std::string, std::string>>* out) {
  out->clear();
  auto it = NewIterator();
  for (it->Seek(start); it->Valid() && out->size() < limit; it->Next()) {
    out->emplace_back(std::string(it->key()), std::string(it->value()));
  }
  return it->status();
}

DBStats DB::GetStats() const {
  DBStats st;
  st.user_bytes_written = user_bytes_.load();
  st.wal_bytes = wal_bytes_.load();
  st.flush_bytes = flush_bytes_.load();
  st.compaction_bytes = compaction_bytes_.load();
  st.manifest_bytes = manifest_bytes_.load();
  st.flushes = flushes_.load();
  st.compactions = compactions_.load();
  st.write_stalls = write_stalls_.load();
  auto v = CurrentVersion();
  st.num_tables = v->tables.size();
  for (const auto& t : v->tables) st.table_bytes += t->reader->file_size();
  st.block_reads = read_stats_.block_reads.load();
  st.bloom_checks = read_stats_.bloom_checks.load();
  st.bloom_negatives = read_stats_.bloom_negatives.load();
  st.bloom_false_positives = read_stats_.bloom_false_positives.load();
  return st;
}

}  // namespace lsm
