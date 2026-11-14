| Step | What changed | tick-to-trade p50 | p99 | p99.9 | book stage p50 | throughput |
|---|---|---:|---:|---:|---:|---:|
| v0_naive | Baseline: std::map book, allocate+decode each message, text order build | 762 ns | 5.33 µs | 59.90 µs | 48 ns | 2.08M msg/s |
| v1_flat_book | Flat price-level arrays + preallocated order pool (Fibonacci-hashed order map) | 704 ns | 5.08 µs | 52.51 µs | 124 ns | 2.46M msg/s |
| v2_inplace_parse | Zero-copy in-place ITCH parsing | 653 ns | 7.28 µs | 68.92 µs | 118 ns | 3.55M msg/s |
| v3_preserialized | Pre-serialized order templates (patch 4 fields, send) | 400 ns | 1.64 µs | 31.18 µs | 118 ns | 3.44M msg/s |
| v4_identity_hash | Identity hash for order refs: live orders stay in a cache-friendly window | 310 ns | 1.52 µs | 27.90 µs | 31 ns | 4.63M msg/s |
| v5_hugepages | Transparent huge pages + prefaulted tables | 316 ns | 1.61 µs | 32.41 µs | 31 ns | 4.32M msg/s |
| compiler:o2 | -O2, generic x86-64-v2, no LTO | 381 ns | 1.79 µs | 40.61 µs | 36 ns | 3.88M msg/s |
| compiler:o3n | -O3 -march=native | 377 ns | 1.72 µs | 41.02 µs | 35 ns | 4.10M msg/s |
| compiler:lto | + link-time optimization | 397 ns | 2.13 µs | 50.46 µs | 37 ns | 2.92M msg/s |
| compiler:pgo | + PGO (trained on synthetic data, measured on the real file) | 393 ns | 2.08 µs | 50.87 µs | 38 ns | 2.66M msg/s |

| UDP receive mode | orders | kernel RX → send() p50 | p99 | p99.9 | in-process p50 |
|---|---:|---:|---:|---:|---:|
| blocking | 3474 | 1.62 ms | 17.04 ms | 29.10 ms | 6.77 µs |
| spin | 953 | 129.02 µs | 9.18 ms | 11.67 ms | 6.77 µs |
| uring | 1012 | 86.02 µs | 10.22 ms | 12.45 ms | 8.41 µs |
