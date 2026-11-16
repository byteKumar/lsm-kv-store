# lsm-kv-store

A persistent key-value storage engine in C++20, built as a log-structured merge tree (the design behind LevelDB, RocksDB and Cassandra's storage layer), with a small TCP server.

Planned pieces: skiplist memtable, write-ahead log with CRC32 checksums and crash recovery, SSTables with a block index and Bloom filters, size-tiered compaction on a background thread, range scans, and a text protocol over TCP.

Work in progress. Build instructions, benchmarks and crash-test results will be added as the milestones land.
