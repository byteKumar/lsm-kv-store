# lsm-kv-store

A persistent key-value storage engine in C++20, built as a log-structured merge tree (the design behind LevelDB, RocksDB and Cassandra's storage layer), with a small TCP server.

- **Write-ahead log** with CRC32 checksums (ARMv8 CRC instructions), configurable sync (`none`, `fsync`, macOS `F_FULLFSYNC`), torn-tail recovery.
- **Skiplist memtable** with lock-free reads (one writer, many readers, acquire/release publishing).
- **SSTables**: 4 KB data blocks, a sparse block index, a per-table **Bloom filter** (10 bits/key by default), a CRC on every block.
- **Read path**: memtable, frozen memtable, then SSTables newest to oldest; tombstones for deletes.
- **Range scans** through a merging iterator, point-in-time via sequence-number snapshots.
- **Size-tiered compaction** on a background thread (separate from the flush thread); drops overwritten versions, and tombstones once no older table remains.
- **Atomic manifest** (write, fsync, rename, fsync dir), orphan cleanup, a `flock` so only one process opens a database.
- **TCP server** with a text protocol (`GET`, `PUT`, `DEL`, `SCAN`), one thread per connection.

## Results

Apple M3, 16 GB, internal SSD, macOS 26.6.2. Single thread, WAL sync `none` unless noted. Each number is from one run. Details, caveats and raw output are in [RESULTS.md](RESULTS.md).

| 10M keys, 16 B keys, 100 B values | |
|---|---|
| Sequential / random writes | 525,023 / 394,026 writes/s |
| Random reads, existing keys | 111,419/s on the first, mostly cold pass; ~505k/s with a warm cache |
| Random reads, missing keys | 3,565,132/s with Bloom filters, 155,336/s without |
| Disk block reads per missing key | 0.0324 with Bloom, 4.0000 without |
| Bloom false-positive rate | 0.809% measured vs 0.819% theory (10 bits/key) |
| 1,000-key range scan | p50 0.112 ms, p99 0.127 ms |
| Write amplification | 4.59 including WAL, 3.38 excluding |
| Durable writes (`F_FULLFSYNC` per write) | 336 writes/s, p50 3.0 ms |
| kill -9 crash test, `--sync full` | 100 kills, 29,531 acknowledged puts, 0 lost |
| Tests | 89 GoogleTest tests passing under normal, ASan + UBSan + LSan and TSan builds |

## Build and test

Needs CMake and a C++20 compiler. GoogleTest is downloaded by CMake.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
```

```bash
ctest --test-dir build -j8
```

All tests in three builds (normal, ASan + UBSan + LeakSanitizer, TSan):

```bash
./scripts/test_all.sh
```

On macOS 26.6 the ASan/TSan runtimes shipped with Apple clang 17 crash at startup (even for hello-world), so the sanitizer builds use Homebrew LLVM (`brew install llvm`); override with `SAN_CXX=...`.

## Run the server

```bash
./build/lsm_server --dir /tmp/lsm-demo --port 7070 --sync full
```

```text
$ nc localhost 7070
PUT city boston
OK
GET city
VALUE boston
SCAN "" 10
city boston
END
DEL city
OK
```

`OK` is sent only after the write is in the WAL and synced per `--sync`. Keys cannot contain spaces; a value is the rest of the line. The server listens on 127.0.0.1 unless `--host` is given.

## Benchmarks and crash test

```bash
./build/lsm_bench --dir bench-data
```

```bash
python3 scripts/crash_test.py --server build/lsm_server --dir /tmp/lsm-crash --kills 100 --sync full
```

The benchmark loads 10M keys (16 B keys, 100 B values) twice (sequential, then random order) and needs about 3 GB of free disk at its peak; it deletes its data when done. The crash test kills the server with SIGKILL at random moments and checks every acknowledged write after each restart.

## Layout

| Path | What |
|---|---|
| `src/skiplist.h`, `src/memtable.*` | memtable |
| `src/wal.*`, `src/crc32.*` | write-ahead log and checksums |
| `src/sstable.*`, `src/bloom.*` | table format and Bloom filters |
| `src/db.*`, `src/manifest.*`, `src/merger.*` | the engine: write/read paths, flush, compaction, recovery, iterators |
| `src/server.*`, `tools/lsm_server.cc` | TCP server |
| `tools/lsm_bench.cc`, `scripts/crash_test.py` | measurements |
| `tests/` | GoogleTest suites |
| `bench/results/` | raw output of the runs reported in RESULTS.md |
