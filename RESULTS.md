# Results

Every number below comes from a single run, reported as measured. Raw output is in [`bench/results/`](bench/results/). Nothing was repeated to pick a best run.

## Machine

- Apple M3, 8 cores (4 performance + 4 efficiency), 16 GB RAM
- Internal Apple SSD AP0256Z (256 GB, APFS), about 13 GB free during the runs
- macOS 26.6.2, Apple clang 17 (`-O3`, Release) for benchmarks, Homebrew LLVM 23.1.5 for the sanitizer builds
- Shared machine: before each timed run I checked that no other benchmark, load test or training job was running (load average about 2, only interactive apps)

## Tests

- **89 GoogleTest tests, all passing**, in three builds: normal (Release), ASan + UBSan + LeakSanitizer, and TSan (`scripts/test_all.sh`).
- **Sanitizer findings in project code: none.** ASan, UBSan, LeakSanitizer and TSan reported no issues at any milestone. The DB, compaction and server concurrency tests were also repeated 5-10 times under TSan with no reports.
- **Toolchain problem found and worked around:** on macOS 26.6.2 the ASan and TSan runtimes shipped with Apple clang 17 (Command Line Tools 26.2) hang or segfault during startup, even for hello-world (`__sanitizer::get_dyld_hdr` → `dyld_shared_cache_iterate_text`). The sanitizer builds use Homebrew LLVM 23.1.5 instead, which works and also supports leak detection on Apple Silicon.
- **Bugs found by tests (not by the sanitizers), and fixed:**
  1. Write-stall deadlock (milestone 6). Writers wait when there are too many tables, but size-tiered compaction only merges runs of similar-size tables. Tables whose sizes each differ by about 2× never form a run, so writers waited forever. Found when `ConcurrentLoadWithCompaction` hung; diagnosed by running each test under a watchdog and sampling the stuck process (all writers inside `DB::Write`). Fix: at the stall limit, compaction merges the cheapest group of adjacent tables. Regression test: `ForcedMergeAtStallLimit`.
  2. Compaction block reads were counted as lookup block reads, which made `BlockReadsAndBloomStats` fail intermittently and would have skewed the Bloom numbers below. Fix: compaction reads are counted separately.

## Throughput: 10M keys, 16 B keys, 100 B values

`./build/lsm_bench --dir bench-data`. Single thread. WAL sync `none` (each record is written with `write(2)`, no fsync), the same default as RocksDB. 64 MB memtable, 4 KB blocks, 10 bits/key Bloom filters, compaction on. Raw output: [`bench_10M.txt`](bench/results/bench_10M.txt).

| Workload | Result |
|---|---|
| Sequential writes (fresh DB) | **525,023 writes/s** (19.05 s); background flush/compaction catch-up afterwards 1.25 s |
| Random-order writes (fresh DB, same keys shuffled) | **394,026 writes/s** (25.38 s); catch-up 0.56 s |
| Random reads, existing keys (1M) | **111,419 reads/s**, 1.0196 block reads per lookup |
| Random reads, missing keys (1M, Bloom on) | **3,565,132 reads/s**, 0.0324 block reads per lookup |
| Range scan of 1,000 keys (10,000 scans) | **p50 0.112 ms, p99 0.127 ms** (max 0.177 ms) |

After the random load the database had 4 tables (1.36 GB). Reads ran on that database after reopening it.

**Cold vs warm cache (read this before quoting the hit number).** The hit benchmark is the first workload after reopening, and it ran mostly against cold data. The missing-key and scan workloads ran afterwards, against a warm file cache. A separate diagnostic run ([`cache_check_10M.txt`](bench/results/cache_check_10M.txt), `--mode cache-check`: same load, then the hit benchmark three times in a row) measured:

| Pass | reads/s |
|---|---|
| 1 (first after reopen) | 69,966 |
| 2 | 503,412 |
| 3 | 508,281 |

So reading existing keys runs at about 0.5M reads/s once the 1.36 GB dataset is in the OS cache, and at 70k-111k reads/s on the first cold pass. The first-pass number also varied between runs (111,419 vs 69,966). The 111,419 figure in the table above is the one from the suite run and is the one reported in the template.

## Bloom filter effect (10 bits per key)

Same database files, reopened once with filters used and once with filters ignored (`Options::use_bloom = false`). 1M lookups of keys that are absent but fall inside every table's key range (odd numbers between the loaded even ones).

| | Bloom off | Bloom on |
|---|---|---|
| Missing-key reads/s | 155,336 | 3,565,132 (23.0× faster) |
| Data blocks read per lookup | 4.0000 | 0.0324 (124× fewer) |

- **False-positive rate:** measured **0.809%** (32,361 of 3,999,998 filter checks) vs theoretical **0.819%** (k = 7 probes, (1 − e^(−k·n/m))^k).
- The hit-read line in the raw output prints `measured_fp_rate=0.00579`. That figure used the wrong denominator: it included the one check per lookup that correctly says "present". Recomputed from the same raw counters, 19,587 / (3,384,071 − 1,000,000) = **0.822%**. The formula is fixed in `lsm_bench.cc` for future runs; the run was not repeated.
- "Disk block reads" means data blocks our engine read with `pread` (there is no block cache). Both runs were served from a warm OS file cache, so the speedup is mostly CPU and syscall work avoided, not SSD latency avoided.

## Write amplification

Bytes written to disk divided by bytes written by the user (key + value = 116 B × 10M = 1.16 GB), after loading 10M keys and letting flush and compaction finish.

| Load | Including WAL | Excluding WAL |
|---|---|---|
| Sequential | **4.59** | 3.38 |
| Random order | **4.59** | 3.38 |

Breakdown (random load): WAL 1.41 GB, flushes 1.36 GB (34 flushes), compaction 2.56 GB (10 compactions), manifest 3.7 KB. Final size on disk 1.36 GB (1.17× the user bytes: each entry stores sequence number, type and two length fields, plus the index and Bloom filters).

Both loads give byte-identical numbers. That is expected here: keys are unique and fixed size, so every flush writes the same number of same-size entries, and size-tiered compaction picks runs only by table size, so it makes the same choices for both orders. Counts come from the engine's own counters, not from OS I/O statistics.

## Cost of durability per write

2,000 single-threaded writes per mode ([`bench_10M.txt`](bench/results/bench_10M.txt), `sync_*` lines):

| WAL sync mode | writes/s | p50 latency | p99 latency |
|---|---|---|---|
| `none` (write only) | 548,434 | 1.5 µs | 4.5 µs |
| `fsync` | 48,031 | 17.8 µs | 48.5 µs |
| `full` (`F_FULLFSYNC`) | 336 | 3.0 ms | 4.1 ms |

A plain `fsync` taking 18 µs shows that on macOS it does not wait for the drive to flush its cache; `F_FULLFSYNC` does, and costs about 3 ms. **Durability claims in this project rely on `F_FULLFSYNC` (`--sync full`).** There is no group commit, so with `full` every write waits for its own flush.

## Crash test (kill -9)

`scripts/crash_test.py`: 4 client connections write keys, and a write counts as acknowledged only once `OK` arrives. Every 10th key is then deleted. After a random 0.3-1.5 s the server gets SIGKILL. It is restarted and every key with a known outcome is checked: acknowledged puts must return the exact value, acknowledged deletes must be NOT_FOUND. Writes in flight at the kill are not judged.

| Run | Kills | Acked puts | Acked deletes | Lost acked writes | Deletes undone | Wrong values | Recovery p50 / max |
|---|---|---|---|---|---|---|---|
| **`--sync full`, 256 KB memtable** (the spec's run) | 100 | 29,531 | 2,925 | **0** | 0 | 0 | 24 / 32 ms |
| `--sync full`, 16 KB memtable | 100 | 29,615 | 2,930 | 0 | 0 | 0 | 24 / 31 ms |
| `--sync full`, 16 KB memtable, instrumented | 100 | 29,464 | 2,918 | 0 | 0 | 0 | 23 / 33 ms |
| `--sync none`, 16 KB memtable | 20 | 814,378 | 81,422 | 0 | 0 | 0 | 4 / 6 ms |

What these runs do and don't show:

- **Coverage:** in the first run a flush happened only every ~3 s of writing, so almost all kills landed during WAL appends. The 16 KB runs flush every ~0.2 s. The instrumented run read `flush_running` / `compaction_running` from STATS just before each kill. Every one of its 100 runs flushed at least once and 90 compacted, but **only 15 kills landed during a flush and 1 during a compaction** (compacting tiny tables takes milliseconds). The `sync=none` run had 4 kills during a flush and 6 during a compaction.
- **What `kill -9` tests:** it kills the process, not the machine. Data already passed to `write(2)` survives in the OS cache, so the `sync=none` run also lost nothing. These tests check the WAL format, recovery, manifest ordering and file cleanup. They do **not** test power loss, which is what `F_FULLFSYNC` protects against. Power-loss durability is argued from the design (sync the WAL per write, sync tables and the manifest before the old WAL is deleted, sync the directory after creates and renames), not measured.
- In the 256 KB run the table count stayed high (35-39) between restarts. Each restart turns the replayed WAL into a small table, and their sizes vary 5× with the random run length, so they rarely form a size-tiered run. The stall-limit fallback (40 tables) kept the count bounded.

## Results template (spec section 5)

```
MACHINE: chip=Apple M3 cores=8 (4P+4E) RAM=16 GB, disk=internal Apple SSD AP0256Z 256 GB, APFS, macOS 26.6.2
TESTS: 89 tests, all passing? yes (normal, ASan+UBSan+LSan, TSan)  ASan/TSan issues found and fixed: 0 in project code (Apple clang 17's ASan/TSan runtimes crash on macOS 26.6, so sanitizer builds use Homebrew LLVM 23)
10M keys, 16 B keys / 100 B values:
  sequential writes/s=525,023  random writes/s=394,026   (1 thread, WAL sync=none)
  random reads/s (hit)=111,419 (first pass after reopen, mostly cold cache; warm-cache passes in a separate run: 503,412 and 508,281)  random reads/s (miss)=3,565,132
  1,000-key scan p50/p99 (ms)=0.112 / 0.127
BLOOM (10 bits/key): miss reads/s off=155,336 on=3,565,132 ; disk reads per miss off=4.0000 on=0.0324 ; FP rate measured=0.809% theory=0.819%
WRITE AMPLIFICATION after 10M keys: 4.59 including WAL (3.38 excluding WAL); same for sequential and random loads
CRASH TEST: kills=100 lost acknowledged writes=0 (sync=full / F_FULLFSYNC, 29,531 acked puts + 2,925 acked deletes; 2 more 100-kill runs with a 16 KB memtable and a 20-kill sync=none run also lost 0)
ANYTHING THAT FAILED OR SURPRISED YOU: Apple clang's ASan/TSan crash at startup on macOS 26.6 (switched to Homebrew LLVM). Write-stall deadlock when table sizes never formed a size-tiered run (fixed with a forced merge at the stall limit). Compaction reads were inflating per-lookup block counts (now counted apart). Hit reads are cache-bound: 70k-111k/s cold vs ~505k/s warm. Plain fsync on macOS costs 18 us (doesn't flush the drive), F_FULLFSYNC 3 ms, i.e. 336 durable writes/s with no group commit. kill -9 can't tell sync on from off (sync=none also lost 0), so power-loss durability is not measured. Only 1 of 100 instrumented kills landed mid-compaction. The hit-read FP rate in the raw output used a wrong denominator (0.579% printed, 0.822% correct).
REPO pushed? yes
```
