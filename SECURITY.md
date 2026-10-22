# Security notes

The software in this repository is experimental and is meant for trusted networks and research use.

## Known properties

- **Authentication and encryption.** `rdmaduct` encrypts and authenticates peers only on contexts created with
  `tls` (`TLS=1` builds: a TLS 1.3 handshake, certificates, then AES-256-GCM per message, on TCP and RDMA); without
  it, nothing is encrypted or authenticated.
  `arenakvd` authenticates clients of every protocol when started with `--aclfile` (users with SHA-256 password
  hashes and per-user rights over commands, categories and key patterns) or `--auth-file` (credentials of earlier
  versions; every user has all rights), and encrypts the memcached and RESP listeners with TLS 1.2 or newer in builds
  with `TLS=1`. With `--native-tls` the native protocol and the replication links use encrypted rdmaduct endpoints;
  otherwise they are not encrypted. Without either file any client that can connect has full access.
- **Remote memory access.** Over RDMA, each connection exposes a 4 KiB page of the endpoint (the flow-control
  credit word) to the peer with remote-write access. Data buffers are registered for local access only. Remote keys
  of some RDMA hardware have been shown to be predictable in the literature (for example the ReDMArk study);
  therefore do not expose RDMA endpoints to untrusted peers.
- **Resource use.** The server pre-registers buffers per native connection and buffers up to `2 * max-entry + 1 MiB`
  of input per text connection. `--max-conns` and `--idle-timeout` bound connections; there is no rate limiting.
- **Hash flooding.** The engine seeds its key hash randomly per instance unless a seed is configured; a fixed seed
  makes collisions predictable.
- **Parsing.** The handshake, the TCP framing and the control frames of rdmaduct, the memcached, RESP and native
  protocol handlers, the snapshot and append-only log loaders, and the ACL parser of arenakv are validated and
  fuzzed: random-input tests in the test suites, and coverage-guided libFuzzer harnesses under AddressSanitizer and
  UBSan (`make fuzz`, run in CI). Fuzzing found and led to fixes of signed overflows in expiry arithmetic. The
  sources pass the clang static analyzer and cppcheck without findings (`make analyze`, in CI).
- **Hardening.** Default builds use `-D_FORTIFY_SOURCE=3`, the stack protector, stack clash protection, CET (x86-64)
  or PAC/BTI (aarch64), PIE, full RELRO and a non-executable stack; CI checks the executables
  (`scripts/check_hardening.sh`).
- **Review status.** The threat model is in [`docs/THREAT_MODEL.md`](docs/THREAT_MODEL.md). No external security
  review has taken place; the tests and tools above do not replace one.
- **One-sided operations.** Regions registered with `DUCT_ACCESS_REMOTE_*` rights are readable or writable by any peer
  that holds their descriptor; on RDMA the network adapter enforces range and rights, on TCP the target library does.

## Reporting a vulnerability

Report suspected vulnerabilities privately to the maintainer (contact in the repository profile or through the
hosting platform's private security advisory feature) rather than in a public issue. Include the affected component,
a reproduction and the observed impact.
