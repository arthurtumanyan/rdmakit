# Administering arenakvd

`arenakvd` is an experimental cache server. This guide covers deployment, sizing, security, persistence,
monitoring, logging, maintenance and troubleshooting. Starting the server, every option, and the commands of each
protocol are described in [`USAGE.md`](USAGE.md).

## Contents

- [Deployment](#deployment)
- [Sizing](#sizing)
- [Threads, CPUs and connections](#threads-cpus-and-connections)
- [Eviction and expiry](#eviction-and-expiry)
- [Authentication](#authentication)
- [TLS](#tls)
- [Network exposure](#network-exposure)
- [Persistence](#persistence)
- [Monitoring](#monitoring)
- [Logging](#logging)
- [Maintenance](#maintenance)
- [Troubleshooting](#troubleshooting)
- [Compatibility notes](#compatibility-notes)

## Deployment

### systemd

[`../contrib/arenakvd.service`](../contrib/arenakvd.service) runs the server as the user `arenakv`, with its state
in `/var/lib/arenakv`, an unlimited memlock limit for RDMA and a hardened sandbox.

```
sudo make -C arenakv install                      # /usr/local/bin/arenakvd (add TLS=1 for TLS listeners)
sudo useradd --system --home-dir /var/lib/arenakv --shell /usr/sbin/nologin arenakv
sudo install -d -m 750 /etc/arenakv
sudo install -m 640 arenakv/contrib/arenakvd.conf /etc/arenakv/arenakvd.conf
sudo install -m 644 arenakv/contrib/arenakvd.service /etc/systemd/system/arenakvd.service
sudo systemctl daemon-reload
sudo systemctl enable --now arenakvd
systemctl status arenakvd
journalctl -u arenakvd -f                         # the log, unless log-file is set
```

`systemctl stop` sends `SIGTERM`: the server stops accepting, writes the snapshot (if configured) and exits; the unit
allows 120 s for that (`TimeoutStopSec`). `systemctl reload` sends `SIGHUP`, which reopens the log file; it does
not reread the configuration. Configuration changes take effect with `systemctl restart`.

### Container

```
docker build -f arenakv/contrib/Dockerfile -t arenakvd .          # from the repository root
docker run -d --name arenakvd -p 6379:6379 -p 11211:11211 -p 7600:7600 \
    -v arenakv-data:/var/lib/arenakv arenakvd --capacity 4G
```

The image listens on 6379 (RESP), 11211 (memcached) and 7600 (native protocol over TCP) on all addresses, and keeps
its snapshot in the volume at `/var/lib/arenakv`. Arguments after the image name are appended to its command line.
`docker stop` sends `SIGTERM` and waits 10 s by default; with large snapshots use `docker stop -t 120`. The image
serves the native protocol over TCP only: RDMA in a container needs the host's RDMA devices
(`--device /dev/infiniband`), `--network host`, `--ulimit memlock=-1` and `--transport rdma`.

### RDMA hosts

- The native listener needs the address of the interface that belongs to the RDMA device (`rdma link show`,
  `ip -4 addr show dev <netdev>`), given as `--addr`, and `--transport rdma` (or `auto`).
- Memory registration counts against the memlock limit. Each native RDMA connection registers
  `2 x depth x max-msg` bytes of buffers (2 MiB with the defaults) plus a 4 KiB page; set `LimitMEMLOCK=infinity`
  (systemd) or `ulimit -l unlimited`.
- Without RDMA hardware, Soft-RoCE provides the same interface for testing:
  `sudo modprobe rdma_rxe && sudo rdma link add rxe0 type rxe netdev <interface>`.

### Several instances on one host

Each instance needs its own ports, snapshot file and log file. Bind them to separate CPU sets (`taskset -c` or
`CPUAffinity=` in systemd) so that the default number of worker threads follows the CPUs each instance may use.

## Sizing

- `--capacity` is the memory of the entry arenas. Resident memory approaches this value once the log has wrapped.
- The hash index is additional memory. It grows with the number of entries (about 20 to 40 bytes per entry) and is
  reported as `index_memory` (RESP `INFO`) and `index_bytes` (memcached `stats`). `--avg-entry` presizes it and
  avoids growth pauses (a growing shard rehashes while holding its lock), at the cost of index memory for keys
  that never arrive.
- `--max-entry` bounds key + value (default 1 MiB). Entries larger than half of a shard's arena are refused as well:
  with capacity C and S shards the limit is `min(max-entry, C / S / 2)`. Large entries with a small capacity need
  fewer shards.
- Native connections reserve `(recv depth + send depth) x max-msg` bytes of buffers each (2 MiB with the
  defaults; RDMA: registered memory). Text connections buffer at most `2 x max-entry + 1 MiB` of input per command
  and stop reading while more than 4 MiB of replies wait to be written.
- Total memory is therefore about `capacity + index + connections x per-connection buffers`.

## Threads, CPUs and connections

- `--threads N` worker threads (default: the CPUs of the process's affinity mask, so `taskset`, `CPUAffinity=` and
  container CPU sets limit it) serve all connections from epoll loops; a connection stays on one worker. Requests of
  a connection are executed in order and may be pipelined.
- `--max-conns N` closes connections above the limit right after accepting them (counted as `rejected_connections`).
  The process also needs a file descriptor per connection (`LimitNOFILE`, `ulimit -n`).
- `--idle-timeout SECONDS` closes connections without traffic, which frees the buffers of abandoned clients.
- RDMA: `--wait event` (default) sleeps on completion events; `--wait poll` spins and only pays off with a core per
  connection.

## Eviction and expiry

- When memory is full, the oldest entries in write order are evicted; an entry read since its last write is kept
  once (second chance). `--no-evict` refuses stores instead (`SERVER_ERROR out of memory storing object`, `-OOM`,
  `AKVP_NOMEM`); deleting or expiring the oldest entries makes room again.
- Expiry: memcached `exptime` (seconds, or a Unix time above 30 days), RESP `EX`/`PX`/`EXAT`/`PXAT`/`EXPIRE`, native
  `ttl_ms`. Expired entries are never returned and are reclaimed when a lookup meets them or the log reaches them, so
  `DBSIZE` and `curr_items` may include expired entries for a while.
- Expiry uses a monotonic clock while the server runs: changing the wall clock does not expire entries early or late
  (absolute times are converted when the command arrives). Across a restart from a snapshot the wall-clock time
  between saving and loading counts towards expiry ([Persistence](#persistence)).

## Authentication

`--auth-file FILE` enables authentication for every protocol. The file holds one credential per line:

```
# user:password, or a bare password for the user "default"
app:9f6c3e1b7a2d4e58
ops:4a71c0d2e9b35f86
0d5b8e2f6a9c1347
```

Empty lines and lines starting with `#` are ignored; everything after the first `:` is the password. A file without
any credential stops the start (it would otherwise silently disable authentication). The file is read once at
start; changes take effect after a restart. Keep it readable by the server user only (`chmod 600`, owner
`arenakv`).

| Protocol | How a client authenticates | Before authentication |
|---|---|---|
| RESP | `AUTH password` (user `default`), `AUTH user password`, or `HELLO 2 AUTH user password`; `redis-cli --user app --pass ...`, `redis.Redis(username=..., password=...)` | `NOAUTH Authentication required.` |
| memcached | first command `set <any key> 0 0 <n>` with the data `<user> <password>`, answered `STORED` (the scheme of memcached `-Y`) | `CLIENT_ERROR unauthenticated` |
| native | `AKVP_AUTH` with `user:password` or a password; `akv_cli --auth app:...`, `akvc_auth()` | status `AKVP_NOAUTH` |

Passwords are compared in constant time per entry; failed attempts are counted (`auth_errors`). There are no
per-user permissions: an authenticated client can read, overwrite and delete every key, and run `FLUSHALL`.
Credentials travel in clear text unless the connection uses TLS.

## TLS

TLS needs a build with `make TLS=1` (OpenSSL). `--tls-resp-port` and `--tls-memcached-port` open TLS listeners
next to (or instead of) the plain ones; `--tls-cert` (certificate chain, PEM) and `--tls-key` (private key, PEM)
are required. TLS 1.2 is the minimum version. `--tls-ca FILE` additionally requires client certificates signed by
a CA in FILE.

A test CA and a server certificate:

```
openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj "/CN=arenakv test CA" -keyout ca.key -out ca.crt
openssl req -newkey rsa:2048 -nodes -subj "/CN=cache.example.net" -keyout server.key -out server.csr
printf 'subjectAltName=DNS:cache.example.net,DNS:localhost,IP:127.0.0.1\n' > san.ext
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 365 -extfile san.ext -out server.crt

arenakvd --port off --tls-resp-port 6380 --tls-memcached-port 11212 --tls-cert server.crt --tls-key server.key
```

Checks from a client:

```
$ valkey-cli --tls --cacert ca.crt -h 127.0.0.1 -p 6380 SET t1 secure
OK
$ printf 'version\r\n' | openssl s_client -quiet -CAfile ca.crt -connect 127.0.0.1:11212 -verify_return_error
VERSION 1.6.0-arenakv-0.2.0
$ kvpace --driver resp --port 6380 --tls --cacert ca.crt --sni localhost --duration 1      # TLS=1 build of kvpace
```

The certificate and key are read at start; a renewed certificate takes effect after a restart. The native protocol
over rdmaduct is never encrypted, over TCP or RDMA.

## Network exposure

- Bind the text listeners to the addresses clients use (`--text-addr`) and the native listener with `--addr`; the
  defaults listen on all IPv4 addresses.
- Expose the plain listeners only to trusted networks, use `--auth-file` everywhere, and TLS where traffic crosses
  untrusted links. Over RDMA each connection exposes a 4 KiB flow-control page to the peer with remote-write access
  ([`../../SECURITY.md`](../../SECURITY.md)); do not accept RDMA connections from untrusted hosts.
- There is no rate limiting; `--max-conns` and `--idle-timeout` bound the connections.

## Persistence

### Behavior

- `--snapshot FILE` loads the file at start (if it exists) and writes it at stop (`SIGTERM`/`SIGINT`, unless
  `--no-save-on-stop`), on `SAVE` (synchronous) and `BGSAVE` (background thread), and every `--save-interval`
  seconds when entries changed. The start fails when the directory of FILE is not writable.
- The file is written as `FILE.tmp`, synced, and renamed over `FILE`, and the directory is synced, so a crash leaves
  the previous snapshot intact. A copy of `FILE` taken at any time is therefore a complete snapshot.
- A snapshot copies one shard at a time while holding its lock: it is consistent per shard, not across shards, and
  writes that happen during a snapshot may or may not be included.
- Records keep key, value, client flags and the time to live that remained when the record was copied. At load, the
  wall-clock time since the file was created is subtracted, so entries whose expiry passed while the server was
  stopped are not restored; an entry may expire up to one second plus the duration of the save early. CAS versions
  are reassigned on load.
- The start-up log reports `snapshot loaded: N entries`. Loading into a smaller `--capacity` evicts the oldest
  entries as stores do; with `--no-evict`, entries that do not fit are skipped.

### Backup and restore

```
redis-cli -p 6379 BGSAVE                          # or SAVE; memcached clients: none, use the RESP listener
redis-cli -p 6379 LASTSAVE                        # changes when the background save finished
cp /var/lib/arenakv/arenakv.snap /backup/arenakv-$(date +%F).snap
```

To restore: stop the server (it writes its own snapshot at stop), replace the snapshot file, start the server. To
start empty, move the file away before starting. To discard the cache contents at the next stop, start with
`--no-save-on-stop` or remove the file after stopping.

### Damaged files

Each block of records carries a CRC32C; a damaged or truncated file is refused and the server does not start:

```
[error] snapshot /var/lib/arenakv/arenakv.snap cannot be loaded (Bad message); move it away to start empty
cannot start server: Bad message
```

Move the file away (keep it for analysis) and start again; the server starts empty.

### Format

Little-endian. Header: magic `AKVSNAP1` (8 bytes), version 1 (4), reserved (4), creation time in Unix seconds (8).
Blocks: magic `BLK1` (4), record count (4), payload length (4), CRC32C of the payload (4), payload of records (key
length 2, reserved 2, client flags 4, value length 4, remaining time to live in ms 8 with -1 for none, key, value).
An end block with record count 0 holds the total number of records (8). Definition: `src/srv.h`, `src/snapshot.c`.

## Monitoring

### Interfaces

| Interface | Content |
|---|---|
| RESP `INFO` | the fields below |
| memcached `stats` | the fields below |
| native `STATS` (`akv_cli stats`) | 15 engine counters and 4 server counters ([`PROTOCOL.md`](PROTOCOL.md#operations)) |
| `--stats SECONDS` | a log line: `entries=5 log=0/512 MiB index=128 KiB gets=4 hits=2 puts=5 evictions=0 expired=0 conns=0` |
| health check | RESP `PING` (`+PONG`), memcached `mn` (`MN`), `akv_cli ping` (`PONG`, exit status 0) |

### RESP INFO fields

| Field | Meaning |
|---|---|
| `arenakv_version`, `process_id`, `uptime_in_seconds` | server identity |
| `io_threads_active` | worker threads |
| `connected_clients` | open connections of all protocols |
| `used_memory` | bytes of the log in use (live and not yet reclaimed entries) plus the index |
| `maxmemory` | configured capacity of the arenas |
| `index_memory` | bytes of the hash index |
| `rdb_bgsave_in_progress`, `rdb_last_save_time`, `rdb_last_bgsave_status` | snapshot state (`ok`/`err` of the last attempt) |
| `total_connections_received`, `rejected_connections` | accepted connections; connections closed by `--max-conns` |
| `total_commands_processed` | requests of all protocols |
| `keyspace_hits`, `keyspace_misses` | reads of all protocols that found / did not find the key |
| `evicted_keys` | entries removed to make room (including removals from a full index window) |
| `expired_keys` | entries removed because their expiry passed |
| `db0:keys` | entries, including expired ones not yet reclaimed |

### memcached stats fields

| Field | Meaning |
|---|---|
| `pid`, `uptime`, `time`, `version`, `pointer_size`, `threads` | server identity |
| `curr_connections`, `total_connections`, `rejected_connections` | as in INFO |
| `cmd_get`, `cmd_set`, `cmd_touch`, `cmd_flush` | read, store, touch and flush requests of all protocols |
| `get_hits`, `get_misses` | reads of all protocols that found / did not find the key |
| `auth_errors` | failed authentication attempts |
| `curr_items`, `total_items` | entries; successful stores since the start |
| `evictions` | as `evicted_keys` |
| `expired`, `reclaimed` | entries removed because their expiry passed (the same counter) |
| `bytes`, `limit_maxbytes`, `index_bytes` | log bytes in use, capacity, index bytes |

The native counters (`akv_cli stats`) count at the engine, for all protocols: `gets`, `hits`, `puts`,
`overwrites`, `dels`, `evictions`, `reinsertions` (second chances), `index_evictions`, `rejected` (stores refused:
too large, or full with `--no-evict`), `expired`, `index_grows`.

### What to watch

| Signal | Computation | Meaning |
|---|---|---|
| hit ratio | `get_hits / cmd_get`, or `hits / gets` of the native counters | falls when the cache is too small or keys expire early |
| memory pressure | `evictions` per second relative to `cmd_set` per second | a steady eviction rate is normal for a full cache; a rising ratio means the working set outgrows `--capacity` |
| refused stores | `rejected` (native counters) | entries larger than the limit, or a full cache with `--no-evict` |
| connection limit | `rejected_connections` | `--max-conns` reached |
| failed logins | `auth_errors` | misconfigured clients or probing |
| snapshot health | `rdb_last_bgsave_status`, `rdb_last_save_time`, log lines `snapshot written` / `snapshot ... failed` | |

## Logging

Each line has a UTC timestamp and a level: `2026-10-21T17:08:01Z [info] snapshot loaded: 5 entries`.
`--log-level error|warn|info|debug` selects the levels written; `--log-file FILE` writes to a file instead of
standard error. `SIGHUP` reopens the file, for rotation with logrotate:

```
/var/log/arenakv/arenakvd.log {
    weekly
    rotate 8
    compress
    delaycompress
    missingok
    notifempty
    postrotate
        systemctl kill -s HUP arenakvd.service
    endscript
}
```

Events at level `info`: start-up line with the ports, `snapshot loaded`, `snapshot written: N entries in T s`, `log
reopened`, periodic `--stats` lines, `stopping`. Errors: listeners that cannot be opened, damaged snapshots, failed
snapshot writes, TLS files that cannot be loaded.

## Maintenance

| Task | Procedure |
|---|---|
| Restart without losing data | configure `--snapshot`; `SIGTERM` (`systemctl restart`) writes it and the next start loads it |
| Change capacity, threads, ports or credentials | edit the configuration, restart; the snapshot is loaded into the new capacity |
| Empty the cache | RESP `FLUSHALL`, memcached `flush_all`, `akv_cli flush`; with a snapshot configured, the next save writes the empty state |
| Upgrade | stop (snapshot written), install the new binary, start; snapshot format version 1 is read by every version that writes it |
| Move to another host | copy the snapshot file; expiry continues from the file's creation time |
| Check the version | `arenakvd --version`, RESP `INFO`, memcached `version` |

## Troubleshooting

| Symptom | Cause | Action |
|---|---|---|
| `invalid value '...' for --...` and exit status 2 | an option value outside its range | the message names the accepted values; `arenakvd --help` |
| `... listener on * port N: Address already in use` | another process holds the port | `ss -ltnp 'sport = :N'`; choose another port |
| two instances with the same native port both start | one listens with TCP and one with RDMA (`--transport auto` on a host with an RDMA device); the port spaces are separate | set `--transport` explicitly |
| RDMA clients cannot connect although the server started | `--addr` is not the address of the interface that belongs to the RDMA device | `rdma link show`, `ip -4 addr show dev <netdev>`; use that address on server and client |
| RDMA connections fail with memory errors | memlock limit too low | `LimitMEMLOCK=infinity`, `ulimit -l unlimited` |
| `snapshot ... cannot be loaded (Bad message)` | damaged or truncated snapshot | move the file away and start ([Damaged files](#damaged-files)) |
| `snapshot directory ... is not writable` | wrong path or permissions | create the directory, give it to the server user |
| `TLS options need a build with TLS=1` | TLS listener options on a build without TLS | build with `make TLS=1` |
| clients get `NOAUTH` / `CLIENT_ERROR unauthenticated` / `AKVP_NOAUTH` | `--auth-file` is set | authenticate first ([Authentication](#authentication)) |
| `SERVER_ERROR object too large for cache`, `ERR string exceeds maximum allowed size`, `AKVP_TOOBIG` | entry above `min(max-entry, capacity / shards / 2)`, or a native message above `--max-msg` | raise `--max-entry` or `--max-msg`, or lower `--shards` |
| `SERVER_ERROR out of memory storing object`, `OOM ...`, `AKVP_NOMEM` | full cache with `--no-evict` | delete entries, raise `--capacity`, or allow eviction |
| hit ratio falls although keys were stored | eviction (see `evictions`) or expiry (`expired`) | raise `--capacity` or the times to live |
| `rejected_connections` grows | `--max-conns` reached | raise the limit, check clients for connection leaks |

## Compatibility notes

- RESP: string commands only; no lists, hashes, sets, streams, scripting, transactions (`MULTI`), pub/sub or
  cluster mode. `HELLO 3` (RESP3) is answered with `NOPROTO`, which clients treat as "use RESP2". `SET ... GET`,
  `GETSET`, `GETDEL` and `APPEND` on a missing key are not atomic with respect to concurrent clients.
- memcached: no binary protocol, no delayed `flush_all`, no meta flags `b`, `I`, `E` and no `N` on `mg`. `version`
  reports `1.6.0-arenakv-<version>` (the protocol level of the meta commands).
- Verified clients (`make compat`, needs Docker): valkey-cli, valkey-benchmark, memtier_benchmark (RESP and memcached
  text), redis-py.
