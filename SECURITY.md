# Security notes

The software in this repository is experimental and is meant for trusted networks and research use.

## Known properties

- **No authentication or encryption.** `rdmaduct` does not authenticate peers or encrypt traffic on either transport.
  `arenakvd` accepts any client that can connect; such a client can read, overwrite and delete any entry.
- **Remote memory access.** Over RDMA, each connection exposes a 4 KiB page of the endpoint (the flow-control
  credit word) to the peer with remote-write access. Data buffers are registered for local access only. Remote keys
  of some RDMA hardware have been shown to be predictable in the literature (for example the ReDMArk study);
  therefore do not expose RDMA endpoints to untrusted peers.
- **Resource use.** The server uses one thread per connection and pre-registers buffers per connection. A client
  that opens many connections can exhaust threads and memory; there are no connection limits or rate limits.
- **Hash flooding.** The engine seeds its key hash randomly per instance unless a seed is configured; a fixed seed
  makes collisions predictable.
- **Parsing.** The handshake and the TCP framing are validated and fuzzed (see `rdmaduct/tests`); the server validates
  request lengths. These tests do not constitute a security audit.

## Reporting a vulnerability

Report suspected vulnerabilities privately to the maintainer (contact in the repository profile or through the
hosting platform's private security advisory feature) rather than in a public issue. Include the affected component,
a reproduction and the observed impact.
