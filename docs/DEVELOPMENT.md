# Development guide

## Prerequisites

| Purpose | Packages (Debian/Ubuntu names) |
|---|---|
| build | `build-essential`, `clang-format` (formatting), `clang` (optional second compiler) |
| RDMA | `libibverbs-dev`, `librdmacm-dev`, `rdma-core`, `ibverbs-utils`, `rdmacm-utils`, `perftest` |
| benchmarks | `python3`, `python3-matplotlib`, Docker, `iproute2` |
| sanitizers | gcc or clang with ASan, UBSan and TSan runtimes |
| fuzzing | `clang` (libFuzzer) |
| static analysis | `clang`, `cppcheck` |

Soft-RoCE allows running the RDMA code paths without RDMA hardware:

```
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev <ethernet interface>
rdma link show
ulimit -l unlimited         # allow memory registration in this shell
```

Use the IPv4 address of that interface as `DUCT_TEST_ADDR` and as the address of RDMA listeners and clients.

## Building and testing

```
make                         # all projects
make test                    # all suites; RDMA suites need DUCT_TEST_ADDR (or a detected device)
make test-tcp                # without RDMA libraries
make sanitize                # ASan + UBSan
make -C arenakv TSAN=1 test  # ThreadSanitizer (binaries run with ASLR disabled)
make fuzz FUZZ_TIME=60       # every libFuzzer harness for 60 s under ASan + UBSan
make analyze                 # clang static analyzer and cppcheck; fails on any finding
make format-check            # CI formatting check
scripts/check_hardening.sh arenakv/build/x86_64/arenakvd   # PIE, RELRO, NX stack, canaries, fortify
```

UBSan findings stop the sanitizer builds (`-fno-sanitize-recover=undefined`). Default builds are hardened
(`-D_FORTIFY_SOURCE=3`, stack protector, stack clash protection, CET or PAC/BTI, PIE, full RELRO, non-executable
stack); `HARDEN=0` builds without.

Fuzzing: each harness in `rdmaduct/tests/fuzz` and `arenakv/tests/fuzz` runs on a copy of its seed corpus in
`tests/fuzz/corpus/<name>` (`arenakv/tests/fuzz/make_seeds.py` writes the seeds); new inputs accumulate in
`build/fuzz/corpus-<name>`, and a finding is written to `build/fuzz/<name>-crash-*` and stops the run. Inputs that
found bugs are kept in the seed corpus as regression cases (`regress-*`).

Builds go to `build/<arch>/` inside each project (`build/<arch>-tcp/` for `RDMA=0` builds), so one source tree can
be built for several architectures and both transport configurations side by side.

## Test strategy

| Layer | Technique |
|---|---|
| codecs and primitives | exhaustive or randomized comparison with a reference (tag matching, handshake, hash) |
| engine | reference-model check of results under eviction; sanitizers; multi-threaded stress |
| transport | one executable definition of the semantics (`rdmaduct/tests/suite.h`) run on every transport and wait mode; fault injection through raw sockets; device counters |
| load generator | synthetic drivers with known service times; in-process fake servers per protocol |
| parsers and loaders | coverage-guided fuzzing (libFuzzer) under ASan + UBSan, with bounded inputs and a reference implementation where one exists (glob matching) |
| durability | the server killed with SIGKILL during writes, restarted, and every acknowledged write checked (`arenakv/tests/durability.py`) |
| benchmarks | claims evaluated against the raw data on every verification |

When a bug is found, add the test that fails without the fix before fixing it. Several RDMA ordering bugs in this
repository were found only by running connection cycles thousands of times on aarch64.

## Adding a benchmark suite

1. Add a suite function to `benchmarks/tools/run.py` (use `interleave()` for repetitions and `run_kvpace()` or
   `run_tool()` for execution so that the manifest stays complete).
2. Extend `benchmarks/tools/bench_data.py` to aggregate the new raw files into the summary.
3. Add figures and tables to `benchmarks/tools/report.py` (palette and conventions are described in its docstring).
4. Document the suite in `benchmarks/METHODOLOGY.md`.

## Debugging RDMA code

- `rdma link show`, `rdma statistic show`, and `/sys/class/infiniband/<dev>/ports/1/hw_counters/` show link state and
  counters (receiver-not-ready, retries, sequence errors).
- A work completion with a non-success status after a disconnect is usually a flush; the first error is the
  informative one.
- Receive-not-ready errors mean the sender ran ahead of posted receive buffers; check flow control and `recv_depth`.
- `ulimit -l` limits registered memory; `ibv_reg_mr` failing with `ENOMEM` usually means this limit.
