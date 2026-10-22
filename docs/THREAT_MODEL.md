# Threat model

Scope: `arenakvd` (the server of [`arenakv`](../arenakv)), the `rdmaduct` transport it uses, and the files they
read and write. `kvpace` and the lab programs are tools for trusted operators and are out of scope. This document
names what an attacker at each position can reach, what limits it, and what is not protected. It describes the
code as it is; it is not the result of an external review. Reporting: [`../SECURITY.md`](../SECURITY.md).

## Assets

| Asset | Where |
|---|---|
| cached values and keys | engine memory; the warm-restart file; snapshots and the append-only log on disk |
| credentials | the ACL or credentials file (passwords as SHA-256 hashes in ACL files, in clear text in credentials files of earlier versions); passwords sent by clients |
| availability | worker threads, memory, connections, disk space of the log |
| registered memory | RDMA regions of the native transport (receive buffers, the credit word, regions an application registers for one-sided access) |

## Trust boundaries and entry points

| Entry point | Who can reach it | What it accepts | Protection |
|---|---|---|---|
| RESP listener (`--resp-port`) | any host that can open a TCP connection to it | RESP2 commands | authentication and per-user rights when an ACL or credentials file is configured; TLS with `TLS=1`; `--text-addr` limits the bound address |
| memcached listener (`--memcached-port`) | as above | text and meta commands | as above |
| native listener (`--port`) | as above, over TCP or RDMA (RoCE or InfiniBand) | the binary protocol of `akvproto.h` | authentication and per-user rights; no encryption |
| configuration, ACL and credentials files | local users with write access to them | options, users and rules | file permissions only; the server reads them at start (`ACL LOAD` rereads the ACL file) |
| snapshot, append-only log, warm-restart file | local users with access to the files or directories | the server's own formats | CRC32C per block, magic numbers, length and position checks; file permissions (0600 / 0700 for files the server creates) |

A client that has authenticated is trusted with exactly the rights of its user; a client without authentication
on a server without access control is trusted with everything.

## Threats and mitigations

| Threat | Position | Mitigation | Remaining exposure |
|---|---|---|---|
| reading or changing data without permission | network | `--aclfile`: commands, categories and key patterns per user; `connection` commands are the only ones open before authentication | without `--aclfile` or `--auth-file` every client has full access; `KEYS` and `SCAN` reveal key names to users who may run them, whatever their key patterns |
| password guessing | network | wrong passwords are counted (`auth_errors`) | no rate limiting or lockout; passwords are compared as SHA-256 hashes, so weak passwords are as weak as their entropy |
| stealing a password from a file | local | ACL files may hold only hashes (`#hash`) | SHA-256 without salt is fast to brute-force for short passwords; credentials files of earlier versions hold passwords in clear text |
| eavesdropping, tampering in transit | network | TLS 1.2 or newer on the text listeners (`TLS=1`), with optional client certificates | the native protocol is not encrypted on either transport; passwords sent over it are visible |
| malformed input crashing or corrupting the server | network | bounds checks in every parser; the memcached, RESP and native parsers, the snapshot and log loaders, the ACL parser, and the rdmaduct handshake and TCP framing are fuzzed with libFuzzer under AddressSanitizer and UBSan (`make fuzz`), and the test suites run under AddressSanitizer, UBSan and ThreadSanitizer | fuzzing finds what its harnesses reach; it is not a proof |
| memory exhaustion | network | the cache has a fixed capacity; text connections buffer at most `2 * max-entry + 1 MiB` of input and stop reading above 4 MiB of pending replies; native connections have fixed buffers; `--max-conns` and `--idle-timeout` | every connection costs memory up to these bounds; there is no per-user quota |
| CPU exhaustion | network | key patterns are matched in O(pattern x key) time; requests are served in bounded turns per connection | expensive commands (`KEYS`, `FLUSHALL`, large `MGET`) are limited only by ACL categories |
| hash flooding | network | the key hash is seeded randomly when the engine is created (the server has no option to fix it); a warm restart keeps the seed in the warm-restart file | a local user who can read that file learns the seed |
| damaged or crafted persistence files | local | CRC32C, magic numbers, positions and record lengths are checked; a damaged log stops the start unless `--aof-load-damaged`; a warm-restart file whose header or positions are inconsistent is discarded | a local user who can write these files can still insert valid records with chosen content |
| disk full | local or caused by writes | the log writer retries every second and reports `aof_last_write_status:err`; with `always`, replies wait | with `everysec` and `no`, changes acknowledged while the disk refuses writes are lost in a crash |
| one-sided RDMA access to registered memory | network (RDMA) | each connection exposes only its credit word with remote-write access; application regions are exposed only when registered with remote rights, and the adapter (RDMA) or the library (TCP emulation) checks range and rights | remote keys of some adapters are predictable (the ReDMArk study); a peer that knows a descriptor can access the region within its rights |

## Out of scope

- Hosts or users that control the server machine (root, the server's user, the hypervisor).
- Side channels (timing of cache hits, memory access patterns) beyond the password comparison.
- Denial of service by link saturation.
- The security of OpenSSL, the kernel, libibverbs and librdmacm.

## Hardening checklist

- Configure `--aclfile` with a user per application, `user default off`, and no `@dangerous` or `@admin` rights for
  applications.
- Bind listeners to internal addresses (`--addr`, `--text-addr`) and filter the ports.
- Use the TLS listeners when the network is not trusted; do not expose the native listener there.
- Keep the configuration, ACL file, snapshots and log readable by the server's user only.
- Keep the warm-restart file (it holds the cache contents and the hash seed) readable by the server's user only;
  the server creates it with mode 0600.
- Set `--max-conns` and `--idle-timeout` for the expected client population.
