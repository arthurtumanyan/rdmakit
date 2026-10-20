# Security notes

The software in this repository is experimental and is meant for trusted networks and research use.

## Known properties

- **Authentication and encryption.** `rdmaduct` does not authenticate peers or encrypt traffic on either transport.
  `arenakvd` authenticates clients of every protocol when started with `--auth-file` (shared credentials compared in
  constant time; no per-user permissions: an authenticated client can read, overwrite and delete any entry) and
  encrypts the memcached and RESP listeners with TLS 1.2 or newer in builds with `TLS=1`. The native protocol over
  rdmaduct is never encrypted. Without `--auth-file` any client that can connect has full access.
- **Remote memory access.** Over RDMA, each connection exposes a 4 KiB page of the endpoint (the flow-control
  credit word) to the peer with remote-write access. Data buffers are registered for local access only. Remote keys
  of some RDMA hardware have been shown to be predictable in the literature (for example the ReDMArk study);
  therefore do not expose RDMA endpoints to untrusted peers.
- **Resource use.** The server pre-registers buffers per native connection and buffers up to `2 * max-entry + 1 MiB`
  of input per text connection. `--max-conns` and `--idle-timeout` bound connections; there is no rate limiting.
- **Hash flooding.** The engine seeds its key hash randomly per instance unless a seed is configured; a fixed seed
  makes collisions predictable.
- **Parsing.** The handshake, the TCP framing and the control frames of rdmaduct, and the memcached and RESP parsers of
  arenakv are validated and fuzzed (`rdmaduct/tests`, `arenakv/tests/test_proto.c`); the native server validates
  request lengths. These tests do not constitute a security audit.
- **One-sided operations.** Regions registered with `DUCT_ACCESS_REMOTE_*` rights are readable or writable by any peer
  that holds their descriptor; on RDMA the network adapter enforces range and rights, on TCP the target library does.

## Reporting a vulnerability

Report suspected vulnerabilities privately to the maintainer (contact in the repository profile or through the
hosting platform's private security advisory feature) rather than in a public issue. Include the affected component,
a reproduction and the observed impact.
