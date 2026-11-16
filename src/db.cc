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

struct DB::CompactionJob {
  std::vector<std::shared_ptr<TableHandle>> inputs;  // adjacent, oldest first
  bool includes_oldest = false;  // no older table exists, so tombstones can go
  bool manual = false;
};

bool PickSizeTieredRun(const std::vector<uint64_t>& sizes, const Options& options, size_t* begin,
                       size_t* end) {
  const size_t n = sizes.size();
  if (options.compaction_min_tables < 2 || n < options.compaction_min_tables) return false;
  // Try runs ending at each table, newest first; grow each run toward older
  // tables while every size stays within the ratio.
  for (size_t e = n; e >= options.compaction_min_tables; e--) {
    size_t b = e - 1;
    uint64_t lo = std::max<uint64_t>(sizes[b], 1), hi = lo;
    while (b > 0 && e - b < options.compaction_max_tables) {
      uint64_t s = std::max<uint64_t>(sizes[b - 1], 1);
      uint64_t nlo = std::min(lo, s), nhi = std::max(hi, s);
      if (static_cast<double>(nhi) > options.compaction_size_ratio * static_cast<double>(nlo)) break;
      b--;
      lo = nlo;
      hi = nhi;
    }
    if (e - b >= options.compaction_min_tables) {
      *begin = b;
      *end = e;
      return true;
    }
  }
  // No similar-size run, but so many tables that writers are about to stall.
  // Waiting for a run that may never form would block writers forever (sizes
  // that each differ by 2x never qualify), so merge the cheapest group of
  // adjacent tables instead.
  if (n >= options.stall_table_count) {
    const size_t w = options.compaction_min_tables;
    uint64_t best = UINT64_MAX;
    for (size_t b = 0; b + w <= n; b++) {
      uint64_t total = 0;
      for (size_t i = b; i < b + w; i++) total += sizes[i];
      if (total <= best) {  // ties go to the newer group
        best = total;
        *begin = b;
        *end = b + w;
      }
    }
    return true;
  }
  return false;
}

DB::DB(const Options& options, std::string dir) : options_(options), dir_(std::move(dir)) {}

DB::~DB() {
  {
    std::lock_guard<std::mutex> l(mu_);
    shutting_down_ = true;
  }
  bg_cv_.notify_all();
  compaction_cv_.notify_all();
  done_cv_.notify_all();
  if (flush_thread_.joinable()) flush_thread_.join();
  if (compaction_thread_.joinable()) compaction_thread_.join();
  if (wal_) wal_->Close();
  if (lock_fd_ >= 0) ::close(lock_fd_);  // also releases the flock
}

Status DB::Open(const Options& options, const std::string& dir, std::unique_ptr<DB>* out) {
  std::unique_ptr<DB> db(new DB(options, dir));
  Status s = db->Recover();
  if (!s.ok()) return s;
  db->flush_thread_ = std::thread([p = db.get()] { p->FlushThread(); });
  db->compaction_thread_ = std::thread([p = db.get()] { p->CompactionThread(); });
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

Status DB::WriteTable(Iterator* it, uint64_t number, bool drop_tombstones, uint64_t* file_size,
                      std::shared_ptr<TableHandle>* handle) {
  handle->reset();
  *file_size = 0;
  SSTableOptions opts;
  opts.block_size = options_.block_size;
  opts.bloom_bits_per_key = options_.bloom_bits_per_key;
  std::unique_ptr<SSTableWriter> w;
  std::string path = TableFileName(dir_, number);
  Status s = SSTableWriter::Open(path, opts, &w);
  if (!s.ok()) return s;
  // Keep only the newest version of each key: nothing (no snapshot) can read
  // the older ones. A tombstone is kept unless drop_tombstones says no older
  // table exists that it would still have to hide something in.
  std::string prev;
  bool has_prev = false;
  uint64_t n = 0;
  for (it->SeekToFirst(); it->Valid() && s.ok(); it->Next()) {
    if (has_prev && it->key() == std::string_view(prev)) {
      versions_dropped_++;
      continue;
    }
    prev.assign(it->key());
    has_prev = true;
    if (drop_tombstones && it->type() == ValueType::kDeletion) {
      tombstones_dropped_++;
      continue;
    }
    s = w->Add(it->key(), it->seq(), it->type(), it->value());
    // Give up quickly on shutdown; the inputs are untouched and still live.
    if ((++n & 0xfff) == 0 && shutting_down_.load()) s = Status::IOError("shutting down");
  }
  if (s.ok()) s = it->status();
  bool empty = s.ok() && w->NumEntries() == 0;
  if (s.ok() && !empty) s = w->Finish(options_.sync_mode);
  if (!s.ok() || empty) {
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
  st.next_file_number = next_file_number_;
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
    s = WriteTable(it.get(), next_file_number_++, /*drop_tombstones=*/false, &size, &h);
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
    std::unique_lock<std::mutex> l(mu_);
    // Too many tables means compaction is behind: wait rather than let every
    // read check an ever-growing list.
    if (options_.compaction && version_->tables.size() >= options_.stall_table_count &&
        bg_error_.ok()) {
      write_stalls_++;
      while (version_->tables.size() >= options_.stall_table_count && bg_error_.ok() &&
             !shutting_down_) {
        done_cv_.wait(l);
      }
    }
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

void DB::FlushThread() {
  std::unique_lock<std::mutex> l(mu_);
  while (true) {
    if (version_->imm != nullptr && bg_error_.ok()) {
      l.unlock();
      Status s = FlushImmutable();
      l.lock();
      if (!s.ok()) bg_error_ = s;
      done_cv_.notify_all();
      compaction_cv_.notify_one();  // a new table may complete a run
      continue;
    }
    if (shutting_down_) break;  // a pending flush is finished first
    bg_cv_.wait(l);
  }
}

Status DB::InstallTables(
    const std::function<std::vector<std::shared_ptr<TableHandle>>(
        const std::vector<std::shared_ptr<TableHandle>>&)>& edit,
    bool drop_imm) {
  std::lock_guard<std::mutex> ml(manifest_mu_);
  std::vector<std::shared_ptr<TableHandle>> tables;
  ManifestState st;
  {
    std::lock_guard<std::mutex> l(mu_);
    tables = edit(version_->tables);
    // Logs older than log_number are fully in tables. While a frozen memtable
    // is still waiting for its flush, its log is the oldest one needed.
    uint64_t log_number = (!drop_imm && imm_log_number_ != 0) ? imm_log_number_ : log_number_;
    st = MakeManifestState(tables, log_number);
  }
  // Table-list changes only happen under manifest_mu_, so the list cannot
  // change while the manifest is being written.
  uint64_t mbytes = 0;
  Status s = WriteManifest(dir_, st, options_.sync_mode, &mbytes);
  if (!s.ok()) return s;
  manifest_bytes_ += mbytes;
  std::lock_guard<std::mutex> l(mu_);
  auto nv = std::make_shared<Version>(*version_);
  nv->tables = std::move(tables);
  if (drop_imm) {
    nv->imm = nullptr;
    imm_log_number_ = 0;
  }
  version_ = std::move(nv);
  return Status::OK();
}

Status DB::FlushImmutable() {
  std::shared_ptr<const Version> v;
  uint64_t number, old_log;
  {
    std::lock_guard<std::mutex> l(mu_);
    v = version_;
    number = next_file_number_++;
    old_log = imm_log_number_;
  }
  auto it = NewMemTableIterator(v->imm.get());
  uint64_t size = 0;
  std::shared_ptr<TableHandle> h;
  Status s = WriteTable(it.get(), number, /*drop_tombstones=*/false, &size, &h);
  if (!s.ok()) return s;
  flush_bytes_ += size;
  flushes_++;
  // The new manifest says everything in older logs is in tables. Only after
  // it is durable may the frozen memtable's log go away.
  s = InstallTables(
      [&](const std::vector<std::shared_ptr<TableHandle>>& cur) {
        auto t = cur;
        if (h) t.push_back(h);  // newest end
        return t;
      },
      /*drop_imm=*/true);
  if (!s.ok()) return s;
  ::unlink(LogFileName(dir_, old_log).c_str());
  return Status::OK();
}

bool DB::PickCompaction(CompactionJob* job) const {
  const auto& tables = version_->tables;
  if (manual_compaction_) {
    if (tables.empty()) return false;
    job->inputs = tables;
    job->includes_oldest = true;
    job->manual = true;
    return true;
  }
  if (!options_.compaction) return false;
  std::vector<uint64_t> sizes;
  for (const auto& t : tables) sizes.push_back(t->reader->file_size());
  size_t b, e;
  if (!PickSizeTieredRun(sizes, options_, &b, &e)) return false;
  job->inputs.assign(tables.begin() + static_cast<ptrdiff_t>(b),
                     tables.begin() + static_cast<ptrdiff_t>(e));
  job->includes_oldest = (b == 0);
  job->manual = false;
  return true;
}

void DB::CompactionThread() {
  std::unique_lock<std::mutex> l(mu_);
  while (!shutting_down_) {
    CompactionJob job;
    if (!bg_error_.ok() || !PickCompaction(&job)) {
      compaction_cv_.wait(l);
      continue;
    }
    compacting_ = true;
    l.unlock();
    Status s = RunCompaction(job);
    l.lock();
    compacting_ = false;
    if (job.manual) manual_compaction_ = false;
    if (!s.ok() && !shutting_down_) bg_error_ = s;
    done_cv_.notify_all();
  }
}

Status DB::RunCompaction(const CompactionJob& job) {
  uint64_t number;
  {
    std::lock_guard<std::mutex> l(mu_);
    number = next_file_number_++;
  }
  std::vector<std::unique_ptr<Iterator>> children;
  for (const auto& t : job.inputs) children.push_back(t->reader->NewIterator(/*for_compaction=*/true));
  auto merged = NewMergingIterator(std::move(children));
  uint64_t size = 0;
  std::shared_ptr<TableHandle> out;
  Status s = WriteTable(merged.get(), number, job.includes_oldest, &size, &out);
  if (!s.ok()) return s;
  compaction_bytes_ += size;
  compactions_++;

  // Replace the run with the output, in the same position. A flush may have
  // appended newer tables meanwhile; that does not move the run.
  std::set<uint64_t> inputs;
  for (const auto& t : job.inputs) inputs.insert(t->number);
  s = InstallTables(
      [&](const std::vector<std::shared_ptr<TableHandle>>& cur) {
        std::vector<std::shared_ptr<TableHandle>> t;
        bool placed = false;
        for (const auto& h : cur) {
          if (inputs.count(h->number)) {
            if (!placed && out) t.push_back(out);
            placed = true;
          } else {
            t.push_back(h);
          }
        }
        return t;
      },
      /*drop_imm=*/false);
  if (!s.ok()) return s;
  // Readers that still hold the old version keep their open file
  // descriptors, so unlinking now is safe on POSIX.
  for (uint64_t n : inputs) ::unlink(TableFileName(dir_, n).c_str());
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

Status DB::WaitForIdle() {
  std::unique_lock<std::mutex> l(mu_);
  while (bg_error_.ok() && !shutting_down_) {
    CompactionJob job;
    bool busy = version_->imm != nullptr || compacting_ || manual_compaction_ || PickCompaction(&job);
    if (!busy) break;
    compaction_cv_.notify_one();
    done_cv_.wait(l);
  }
  return bg_error_;
}

Status DB::CompactAll() {
  Status s = Flush();
  if (!s.ok()) return s;
  std::unique_lock<std::mutex> l(mu_);
  // Let a running automatic compaction finish first.
  while (compacting_ && bg_error_.ok()) done_cv_.wait(l);
  if (!bg_error_.ok()) return bg_error_;
  if (version_->tables.empty()) return Status::OK();  // nothing to compact
  manual_compaction_ = true;
  compaction_cv_.notify_one();
  while (manual_compaction_ && bg_error_.ok() && !shutting_down_) done_cv_.wait(l);
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
  st.tombstones_dropped = tombstones_dropped_.load();
  st.versions_dropped = versions_dropped_.load();
  auto v = CurrentVersion();
  st.num_tables = v->tables.size();
  for (const auto& t : v->tables) st.table_bytes += t->reader->file_size();
  st.block_reads = read_stats_.block_reads.load();
  st.compaction_block_reads = read_stats_.compaction_block_reads.load();
  st.bloom_checks = read_stats_.bloom_checks.load();
  st.bloom_negatives = read_stats_.bloom_negatives.load();
  st.bloom_false_positives = read_stats_.bloom_false_positives.load();
  return st;
}

}  // namespace lsm
