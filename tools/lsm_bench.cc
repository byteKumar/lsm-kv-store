// lsm_bench: the measurements from section 4 of the spec.
//
//   lsm_bench --dir DIR [--num 10000000] [--reads 1000000] [--scans 10000]
//             [--scan-len 1000] [--sync-writes 2000] [--seed 1]
//
// Every workload runs once, single-threaded, and prints its result as one
// "RESULT name key=value ..." line. Nothing is repeated or best-of'd.
//
// Keys are 16 bytes: 'k' + 15 zero-padded digits. Loaded keys use even
// numbers (k000000000000000, k000000000000002, ...); "missing" keys use the
// odd numbers in between, so they fall inside every table's key range and
// only the Bloom filter (not the min/max range check) can skip them.
// Values are 100 bytes taken from a random buffer.
//
// The default WAL sync mode for the load benchmarks is "none" (write(2) per
// record, no fsync), like RocksDB's default. A separate workload measures
// what fsync and F_FULLFSYNC per write cost.

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "db.h"

namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Args {
  std::string dir;
  uint64_t num = 10'000'000;
  uint64_t reads = 1'000'000;
  uint64_t scans = 10'000;
  uint64_t scan_len = 1000;
  uint64_t sync_writes = 2000;
  uint64_t seed = 1;
  std::string mode = "full";  // "full" or "cache-check" (diagnostic, see main)
};

double Seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

void MakeKey(uint64_t n, char* buf) { std::snprintf(buf, 17, "k%015" PRIu64, n); }

class Values {
 public:
  explicit Values(uint64_t seed) : data_(1 << 20, '\0') {
    std::mt19937_64 rng(seed);
    for (char& c : data_) c = static_cast<char>('!' + rng() % 94);  // printable, no spaces
  }
  std::string_view Get(uint64_t i) const { return std::string_view(data_).substr((i * 97) % (data_.size() - 100), 100); }

 private:
  std::string data_;
};

void Check(const lsm::Status& s, const char* what) {
  if (!s.ok()) {
    std::fprintf(stderr, "%s: %s\n", what, s.ToString().c_str());
    std::exit(1);
  }
}

std::unique_ptr<lsm::DB> OpenDB(const std::string& dir, lsm::Options o) {
  std::unique_ptr<lsm::DB> db;
  Check(lsm::DB::Open(o, dir, &db), "open");
  return db;
}

uint64_t DirBytes(const std::string& dir) {
  uint64_t total = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.is_regular_file()) total += e.file_size();
  }
  return total;
}

double Percentile(std::vector<double> v, double p) {
  std::sort(v.begin(), v.end());
  size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
  return v[idx];
}

void PrintStats(const char* name, const lsm::DBStats& st) {
  std::printf(
      "STATS %s user_bytes=%" PRIu64 " wal_bytes=%" PRIu64 " flush_bytes=%" PRIu64
      " compaction_bytes=%" PRIu64 " manifest_bytes=%" PRIu64 " flushes=%" PRIu64
      " compactions=%" PRIu64 " write_stalls=%" PRIu64 " tables=%" PRIu64 " table_bytes=%" PRIu64
      "\n",
      name, st.user_bytes_written, st.wal_bytes, st.flush_bytes, st.compaction_bytes,
      st.manifest_bytes, st.flushes, st.compactions, st.write_stalls, st.num_tables,
      st.table_bytes);
}

// Loads keys 0, 2, 4, ... in the given order; returns writes/s.
void Load(const char* name, const Args& a, const std::vector<uint64_t>& order, const Values& vals) {
  std::string dir = a.dir + "/" + name;
  fs::remove_all(dir);
  lsm::Options o;  // sync none, 64 MB memtable, 10 bits/key, compaction on
  auto db = OpenDB(dir, o);
  char key[17];
  auto t0 = Clock::now();
  for (uint64_t i = 0; i < order.size(); i++) {
    MakeKey(2 * order[i], key);
    Check(db->Put(std::string_view(key, 16), vals.Get(order[i])), "put");
  }
  auto t1 = Clock::now();
  // Writes are acknowledged once in the WAL; flushes/compactions may still be
  // running. Measure the catch-up separately instead of hiding it. The final
  // Flush() puts the last memtable into a table too, so write amplification
  // covers all the data and a reopen has no WAL to replay.
  Check(db->Flush(), "flush");
  Check(db->WaitForIdle(), "wait");
  auto t2 = Clock::now();
  lsm::DBStats st = db->GetStats();
  double wa = static_cast<double>(st.DiskBytesWritten()) / static_cast<double>(st.user_bytes_written);
  double wa_no_wal = static_cast<double>(st.flush_bytes + st.compaction_bytes) /
                     static_cast<double>(st.user_bytes_written);
  std::printf(
      "RESULT %s keys=%zu seconds=%.3f writes_per_sec=%.0f background_catchup_seconds=%.3f "
      "write_amp=%.3f write_amp_excl_wal=%.3f dir_bytes=%" PRIu64 "\n",
      name, order.size(), Seconds(t0, t1), static_cast<double>(order.size()) / Seconds(t0, t1),
      Seconds(t1, t2), wa, wa_no_wal, DirBytes(dir));
  PrintStats(name, st);
  std::fflush(stdout);
}

void ReadRandom(const char* name, lsm::DB* db, const Args& a, bool hit, std::mt19937_64* rng) {
  std::uniform_int_distribution<uint64_t> pick(0, a.num - 1);
  char key[17];
  std::string value;
  lsm::DBStats before = db->GetStats();
  uint64_t found = 0;
  auto t0 = Clock::now();
  for (uint64_t i = 0; i < a.reads; i++) {
    uint64_t n = 2 * pick(*rng) + (hit ? 0 : 1);
    MakeKey(n, key);
    lsm::Status s = db->Get(std::string_view(key, 16), &value);
    if (s.ok()) found++;
    else if (!s.IsNotFound()) Check(s, "get");
  }
  auto t1 = Clock::now();
  lsm::DBStats after = db->GetStats();
  uint64_t block_reads = after.block_reads - before.block_reads;
  uint64_t checks = after.bloom_checks - before.bloom_checks;
  uint64_t fps = after.bloom_false_positives - before.bloom_false_positives;
  std::printf(
      "RESULT %s reads=%" PRIu64 " found=%" PRIu64 " seconds=%.3f reads_per_sec=%.0f "
      "block_reads=%" PRIu64 " block_reads_per_lookup=%.4f bloom_checks=%" PRIu64
      " bloom_false_positives=%" PRIu64 " measured_fp_rate=%.5f tables=%" PRIu64 "\n",
      name, a.reads, found, Seconds(t0, t1), static_cast<double>(a.reads) / Seconds(t0, t1),
      block_reads, static_cast<double>(block_reads) / static_cast<double>(a.reads), checks, fps,
      // A found key passes exactly one filter check truthfully (its own
      // table); every other check is on a table without the key.
      checks > found ? static_cast<double>(fps) / static_cast<double>(checks - found) : 0.0,
      after.num_tables);
  std::fflush(stdout);
}

void Scans(lsm::DB* db, const Args& a, std::mt19937_64* rng) {
  std::uniform_int_distribution<uint64_t> pick(0, a.num - a.scan_len);
  std::vector<double> ms;
  ms.reserve(a.scans);
  std::vector<std::pair<std::string, std::string>> out;
  char key[17];
  uint64_t short_scans = 0;
  auto t0 = Clock::now();
  for (uint64_t i = 0; i < a.scans; i++) {
    MakeKey(2 * pick(*rng), key);
    auto s0 = Clock::now();
    Check(db->Scan(std::string_view(key, 16), a.scan_len, &out), "scan");
    auto s1 = Clock::now();
    ms.push_back(Seconds(s0, s1) * 1000.0);
    if (out.size() != a.scan_len) short_scans++;
  }
  auto t1 = Clock::now();
  std::printf(
      "RESULT scan scans=%" PRIu64 " scan_len=%" PRIu64 " short_scans=%" PRIu64
      " p50_ms=%.4f p99_ms=%.4f max_ms=%.4f total_seconds=%.3f\n",
      a.scans, a.scan_len, short_scans, Percentile(ms, 0.50), Percentile(ms, 0.99),
      *std::max_element(ms.begin(), ms.end()), Seconds(t0, t1));
  std::fflush(stdout);
}

// Cost of durability: per-write latency with each sync mode.
void SyncWrites(const Args& a, const Values& vals) {
  for (lsm::SyncMode m : {lsm::SyncMode::kNone, lsm::SyncMode::kFsync, lsm::SyncMode::kFullFsync}) {
    std::string dir = a.dir + "/sync-" + lsm::SyncModeName(m);
    fs::remove_all(dir);
    lsm::Options o;
    o.sync_mode = m;
    auto db = OpenDB(dir, o);
    std::vector<double> us;
    char key[17];
    auto t0 = Clock::now();
    for (uint64_t i = 0; i < a.sync_writes; i++) {
      MakeKey(2 * i, key);
      auto s0 = Clock::now();
      Check(db->Put(std::string_view(key, 16), vals.Get(i)), "put");
      us.push_back(Seconds(s0, Clock::now()) * 1e6);
    }
    auto t1 = Clock::now();
    std::printf("RESULT sync_%s writes=%" PRIu64 " writes_per_sec=%.0f p50_us=%.1f p99_us=%.1f\n",
                lsm::SyncModeName(m), a.sync_writes,
                static_cast<double>(a.sync_writes) / Seconds(t0, t1), Percentile(us, 0.5),
                Percentile(us, 0.99));
    std::fflush(stdout);
    db.reset();
    fs::remove_all(dir);
  }
}

#ifdef __APPLE__
std::string ChipName() {
  char buf[256];
  size_t len = sizeof(buf);
  if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) != 0) return "?";
  return std::string(buf, strnlen(buf, len));
}
#else
std::string ChipName() { return "unknown"; }  // not needed off macOS
#endif

uint64_t NumCores() { return static_cast<uint64_t>(sysconf(_SC_NPROCESSORS_ONLN)); }

uint64_t RamBytes() {
  return static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES)) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; i++) {
    std::string k = argv[i];
    if (i + 1 >= argc) {
      std::fprintf(stderr, "missing value for %s\n", k.c_str());
      return 2;
    }
    std::string v = argv[++i];
    if (k == "--dir") a.dir = v;
    else if (k == "--num") a.num = std::stoull(v);
    else if (k == "--reads") a.reads = std::stoull(v);
    else if (k == "--scans") a.scans = std::stoull(v);
    else if (k == "--scan-len") a.scan_len = std::stoull(v);
    else if (k == "--sync-writes") a.sync_writes = std::stoull(v);
    else if (k == "--seed") a.seed = std::stoull(v);
    else if (k == "--mode") a.mode = v;
    else {
      std::fprintf(stderr, "unknown flag %s\n", k.c_str());
      return 2;
    }
  }
  if (a.dir.empty() || a.num < a.scan_len) {
    std::fprintf(stderr, "usage: lsm_bench --dir DIR [--num N] [--reads N] [--scans N]\n");
    return 2;
  }
  fs::create_directories(a.dir);
  std::printf("MACHINE chip=\"%s\" cores=%" PRIu64 " ram_gb=%.1f\n",
              ChipName().c_str(), NumCores(), static_cast<double>(RamBytes()) / (1ull << 30));
  std::printf("CONFIG num=%" PRIu64 " key_bytes=16 value_bytes=100 reads=%" PRIu64
              " scans=%" PRIu64 " scan_len=%" PRIu64
              " memtable_mb=64 block_size=4096 bloom_bits_per_key=10 sync=none threads=1\n",
              a.num, a.reads, a.scans, a.scan_len);
  std::fflush(stdout);

  Values vals(a.seed);
  std::mt19937_64 rng(a.seed);

  if (a.mode == "cache-check") {
    // Diagnostic only, not part of the reported suite: load the random-order
    // database, reopen it, and run the hit benchmark three times in a row to
    // see how much of the first pass is cold-cache cost.
    std::vector<uint64_t> order(a.num);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), rng);
    Load("fillrandom", a, order, vals);
    order.clear();
    order.shrink_to_fit();
    std::string dir = a.dir + "/fillrandom";
    {
      auto db = OpenDB(dir, lsm::Options{});
      ReadRandom("cachecheck_hit_pass1", db.get(), a, true, &rng);
      ReadRandom("cachecheck_hit_pass2", db.get(), a, true, &rng);
      ReadRandom("cachecheck_hit_pass3", db.get(), a, true, &rng);
    }
    fs::remove_all(dir);
    return 0;
  }

  // 1. Sequential load (fresh DB), then delete it to save disk.
  std::vector<uint64_t> order(a.num);
  std::iota(order.begin(), order.end(), 0);
  Load("fillseq", a, order, vals);
  fs::remove_all(a.dir + "/fillseq");

  // 2. Random-order load of the same keys (fresh DB). Reads run on this one.
  std::shuffle(order.begin(), order.end(), rng);
  Load("fillrandom", a, order, vals);
  order.clear();
  order.shrink_to_fit();

  std::string dir = a.dir + "/fillrandom";
  {
    // Reopen so every read workload starts from the same on-disk state.
    lsm::Options o;
    auto db = OpenDB(dir, o);
    ReadRandom("readrandom_hit", db.get(), a, true, &rng);
    ReadRandom("readrandom_miss_bloom_on", db.get(), a, false, &rng);
    Scans(db.get(), a, &rng);
  }
  {
    lsm::Options o;
    o.use_bloom = false;  // same files, filters ignored
    auto db = OpenDB(dir, o);
    ReadRandom("readrandom_miss_bloom_off", db.get(), a, false, &rng);
  }
  fs::remove_all(dir);

  // 3. What durability costs per write.
  SyncWrites(a, vals);
  return 0;
}
