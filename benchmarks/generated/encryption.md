Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 16 connections, 90% GET. arenakv native: rdmaduct encrypted endpoints (TLS 1.3 handshake, AES-256-GCM per message) over TCP; RESP: TLS 1.3 streams. All arenakv runs use the arenakvd of the TLS=1 build. Ratio: encrypted throughput over unencrypted. Latencies are GET latencies in microseconds.

| Path | Values | ops/s plain | ops/s encrypted | Ratio | Server CPU µs/op plain | Server CPU µs/op encrypted | p99 plain | p99 encrypted |
|---|---|---|---|---|---|---|---|---|
| arenakv native | 64-byte values | 267,096 (cv 0.7%) | 228,235 (cv 0.8%) | 0.85 | 3.75 (cv 0.7%) | 4.38 (cv 0.9%) | 116 (cv 0.4%) | 136 (cv 0.4%) |
| arenakv native | 16 KiB values | 162,115 (cv 1.3%) | 112,010 (cv 0.8%) | 0.69 | 6.17 (cv 1.3%) | 8.92 (cv 0.8%) | 194 (cv 1.1%) | 279 (cv 1.2%) |
| arenakv RESP | 64-byte values | 260,278 (cv 0.7%) | 183,277 (cv 1.0%) | 0.70 | 3.84 (cv 0.7%) | 5.45 (cv 1.1%) | 119 (cv 0.5%) | 169 (cv 0.5%) |
| arenakv RESP | 16 KiB values | 159,174 (cv 0.6%) | 76,108 (cv 0.5%) | 0.48 | 6.28 (cv 0.7%) | 13.14 (cv 0.5%) | 197 (cv 0.3%) | 407 (cv 0.3%) |
| Valkey RESP | 64-byte values | 210,823 (cv 1.1%) | 163,771 (cv 1.1%) | 0.78 | 4.74 (cv 1.1%) | 6.11 (cv 1.1%) | 139 (cv 0.8%) | 180 (cv 1.2%) |
| Valkey RESP | 16 KiB values | 142,592 (cv 0.4%) | 70,485 (cv 1.4%) | 0.49 | 7.01 (cv 0.4%) | 14.19 (cv 1.4%) | 158 (cv 6.7%) | 309 (cv 16.0%) |
