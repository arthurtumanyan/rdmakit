# Operating arenakvd

`arenakvd` is an experimental cache server. It serves three protocols from one engine: the native protocol over
rdmaduct (RDMA or TCP), the memcached text and meta protocol, and RESP2 (the Redis protocol). This guide covers
configuration, security, persistence, monitoring and deployment. Option names are the long command-line options;
every option can also be set in a configuration file.

## Listeners

| Option | Default | Protocol |
|---|---|---|
| `--port PORT` | 7600 | native protocol over rdmaduct; `off` disables it |
| `--transport tcp\|rdma\|auto`, `--addr ADDRESS` | auto, all IPv4 | transport and address of the native listener (RDMA: the address of the RDMA interface) |
| `--memcached-port PORT` | off | memcached text and meta protocol |
| `--resp-port PORT` | off | RESP2 |
| `--text-addr ADDRESS` | all IPv4 | address of the text listeners |
| `--tls-memcached-port`, `--tls-resp-port` | off | the same protocols over TLS (build with `TLS=1`) |

Typical deployment as a drop-in cache for existing clients:

```
arenakvd --port off --resp-port 6379 --memcached-port 11211 --capacity 8G
```

## Sizing

- `--capacity` is the memory of the entry arenas. Resident memory approaches this value once the log has wrapped.
  The hash index is additional; it grows with the number of entries (about 20 to 40 bytes per entry, reported as
  `index_bytes` / `index_memory`). `--avg-entry` presizes it and avoids growth pauses (a growing shard rehashes
  while holding its lock), at the cost of index memory touched by sparse keys.
- `--max-entry` bounds key + value (default 1 MiB). Entries larger than half of a shard arena are refused as well:
  with capacity C and S shards the limit is `min(max-entry, C / S / 2)`.
- `--max-msg` bounds native messages (`key + value + 36` bytes for requests with an extension). Per native
  connection the server reserves `(recv depth + send depth) * max-msg` bytes of buffers (RDMA: registered memory).
- Text connections buffer at most `2 * max-entry + 1 MiB` of input per command and stop reading while more than
  4 MiB of replies wait to be written.

## Eviction and expiry

- When memory is full, the oldest entries in write order are evicted; an entry read since its last write is kept
  once (second chance). `--no-evict` refuses stores instead (`SERVER_ERROR out of memory storing object`, `-OOM`,
  `AKVP_NOMEM`); deleting or expiring the oldest entries makes room again.
- Expiry: memcached `exptime` (seconds, or a Unix time above 30 days), RESP `EX`/`PX`/`EXAT`/`PXAT`/`EXPIRE`, native
  `ttl_ms`. Expired entries are never returned and are reclaimed when a lookup meets them or the log reaches them.
  Expiry uses a monotonic clock: changing the wall clock does not expire entries early or late (absolute times
  are converted when the command arrives).

## Threads and connections

- `--threads N` worker threads (default: online CPUs) serve all connections from epoll loops; a connection stays
  on one worker. Requests of a connection are executed in order and may be pipelined.
- `--max-conns N` closes connections above the limit right after accepting them (`rejected_connections`).
- `--idle-timeout SECONDS` closes connections without traffic.
- RDMA: `--wait event` (default) sleeps on completion events; `--wait poll` spins and only pays off with a core per
  connection. RDMA needs a memlock limit that allows registering the buffers (`ulimit -l`, `LimitMEMLOCK` in
  systemd).

## Security

- Authentication: `--auth-file FILE` holds one credential per line, `user:password`, or a bare password for the
  user `default`. With credentials configured every protocol requires authentication first:
  - RESP: `AUTH password` (user `default`), `AUTH user password`, or `HELLO 2 AUTH user password`;
  - memcached: the first command must be `set <any key> 0 0 <n>` with the data `<user> <password>` (the scheme of
    memcached `-Y`), answered `STORED` on success;
  - native: `AKVP_AUTH` with `user:password` or a password.
  Passwords are compared in constant time per entry. Failed attempts are counted (`auth_errors`).
- TLS (build with `make TLS=1`): `--tls-cert`, `--tls-key` (PEM) enable the TLS listeners; TLS 1.2 or newer.
  `--tls-ca FILE` requires client certificates signed by that CA. The native protocol over rdmaduct is not encrypted;
  use it on trusted networks only.
- The server keeps no per-user permissions: an authenticated client can read, write and delete every key.

## Persistence

- `--snapshot FILE` loads the file at start (if it exists) and writes it at stop (`SIGTERM`/`SIGINT`, unless
  `--no-save-on-stop`), on `SAVE` (synchronous) and `BGSAVE` (background thread), and every `--save-interval`
  seconds when entries changed.
- The file is written as `FILE.tmp`, synced and renamed over `FILE`, so a crash leaves the previous snapshot intact.
  Each block of records carries a CRC32C; a damaged or truncated file is refused at start (the server does not start;
  move the file away to start empty).
- Records keep key, value, client flags and the remaining time to live; CAS versions are reassigned on load.
- A snapshot copies one shard at a time while holding its lock, so it is consistent per shard, not across shards.
  Writes that happen during a snapshot may or may not be included.
- Format (little-endian): header `AKVSNAP1`, version 1, creation time; blocks `BLK1`, record count, payload length,
  CRC32C, payload of records (key length 2, reserved 2, flags 4, value length 4, ttl ms 8, key, value); an end block
  with count 0 holds the total record count. Definition: `src/srv.h`.

## Monitoring

| Interface | Content |
|---|---|
| RESP `INFO` | server, clients, memory (`used_memory`, `maxmemory`, `index_memory`), persistence, stats (connections, commands, hits, misses, evictions, expirations), keyspace |
| memcached `stats` | `curr_connections`, `total_connections`, `rejected_connections`, `cmd_get`, `cmd_set`, `cmd_flush`, `cmd_touch`, `get_hits`, `get_misses`, `curr_items`, `evictions`, `expired`, `bytes`, `limit_maxbytes`, `index_bytes`, `threads`, `auth_errors` |
| native `STATS` | engine counters and server counters (`akvproto.h`) |
| `--stats SEC` | a log line every SEC seconds |

Useful ratios: `get_hits / cmd_get` (hit ratio), `evictions / cmd_set` (memory pressure), `rejected` (stores refused).

## Configuration file and signals

```
arenakvd --config /etc/arenakv/arenakvd.conf
```

Lines are `name = value` (or `name value`; a flag alone or `= yes`), `#` starts a comment, names are the long
options without dashes. Command-line options override the file. Example: `contrib/arenakvd.conf`.

| Signal | Effect |
|---|---|
| `SIGTERM`, `SIGINT` | stop accepting, close connections, write the snapshot (if configured), exit 0 |
| `SIGHUP` | reopen the log file (`--log-file`), for log rotation |

Logging: `--log-level error|warn|info|debug`, `--log-file FILE` (default: standard error), one line per event with a
UTC timestamp.

## Deployment

- systemd: `contrib/arenakvd.service` (runs as user `arenakv`, state in `/var/lib/arenakv`, unlimited memlock).
- Container: `docker build -f arenakv/contrib/Dockerfile -t arenakvd .` from the repository root; the image listens on
  6379 (RESP), 11211 (memcached) and 7600 (native over TCP) and keeps its snapshot in `/var/lib/arenakv`.
- Clients: any Redis or memcached client library for the text protocols; `akvclient.h` (with `akvc_pool` for
  several servers) for the native protocol over RDMA or TCP.

## Compatibility notes

- RESP: string commands only; no lists, hashes, sets, streams, scripting, transactions (`MULTI`), pub/sub or
  cluster mode. `HELLO 3` (RESP3) is answered with `NOPROTO`, which clients treat as "use RESP2". `SET ... GET`,
  `GETSET`, `GETDEL` and `APPEND` on a missing key are not atomic with respect to concurrent clients.
- memcached: no binary protocol, no delayed `flush_all`, no meta flags `b`, `I`, `E` and no `N` on `mg`. `version`
  reports `1.6.0-arenakv-<version>` (the protocol level of the meta commands).
- Verified clients (`make compat`, needs Docker): valkey-cli, valkey-benchmark, memtier_benchmark (RESP and memcached
  text), redis-py.
