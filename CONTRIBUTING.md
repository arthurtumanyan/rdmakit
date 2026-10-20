# Contributing

## Ground rules

- **Evidence first.** Performance claims need raw data produced by `benchmarks/tools/run.py`, committed under
  `benchmarks/data/<run-id>/`, and statements that compare numbers need an entry in `benchmarks/tools/claims.py`.
  `make verify` must pass. Never edit raw data, generated tables or figures by hand.
- **Tests with every behavior change.** The Testing section of each project's README lists the suites that guard each component.
- **Documentation register.** Neutral and precise: state facts, define terms, cite sources for limits and
  protocol behavior. No first-person wording, no marketing language, no speculation presented as fact.
- **No tool attribution** in source, documentation, commit messages or trailers.

## Workflow

1. Build and test: `make && make test` (or `make test-tcp` without RDMA).
2. Format C sources with `clang-format` (`make format`); `make format-check` is part of CI.
3. Run the sanitizers for changes to C code: `make sanitize`.
4. For changes to concurrency or the RDMA transport, run the suites on x86-64 and aarch64 and, when possible, on
   RDMA hardware.
5. Commit with an imperative subject line (about 70 characters) and a body explaining why the change is needed.

## Code style

- C with `-std=gnu2x` (gcc 13+, clang 18+); `-Wall -Wextra -Werror`.
- One declaration and one statement per line.
- Every public function, type, macro and file has a header comment stating behavior, parameters, return values,
  ownership, thread safety and error behavior. Internal functions are commented when their behavior is not obvious.
- Architecture-specific code sits behind a portable fallback and is selected at build or run time; inline assembly
  requires a comment naming the instruction sequence, the architecture and why no intrinsic suffices.

## Reporting problems

Include the commit, the command line, the kernel and compiler versions, whether RDMA hardware or Soft-RoCE was used,
and the full output. For benchmark disputes follow "Challenging a result" in
[`benchmarks/METHODOLOGY.md`](benchmarks/METHODOLOGY.md).
