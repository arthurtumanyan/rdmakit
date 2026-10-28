# Administering arenakvd

`arenakvd` is an experimental cache server. This guide covers deployment, sizing, security, persistence,
monitoring, logging, maintenance and troubleshooting. Starting the server, every option, and the commands of each
protocol are described in [`USAGE.md`](USAGE.md).

## Contents

- [Deployment](#deployment)
- [Sizing](#sizing)
- [Threads, CPUs and connections](#threads-cpus-and-connections)
- [Eviction and expiry](#eviction-and-expiry)
- [Authentication and access control](#authentication-and-access-control)
- [TLS](#tls)
- [Network exposure](#network-exposure)
- [Persistence](#persistence)
- [Replication](#replication)
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
  reported as `index_memory` (RESP `INFO`) and `index_bytes` (memcached `stats`). `--avg-entry` presizes it, at
  the cost of index memory for keys that never arrive. Growth does not pause a shard: the old table is moved into the
  new one a bucket per operation, and until the move ends the shard holds both tables (1.5 times the new size).
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
- `--idle-timeout SECONDS` closes connections without traffic, which frees the buffers of abandoned clients. A
  connection is closed between SECONDS and SECONDS + 1 after its last request (each worker checks once per second).
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

## Authentication and access control

Two options enable authentication for every protocol; they exclude each other.

| Option | File content | Permissions |
|---|---|---|
| `--aclfile FILE` | users with passwords and rules, in a subset of the Valkey ACL file syntax | per user: commands, categories, key patterns, channels |
| `--auth-file FILE` | `user:password` per line, or a bare password for the user `default` (the format of earlier versions) | every user may do everything |

### ACL file

```
# /etc/arenakv/users.acl, mode 600, owner arenakv
user default off
user admin on >9f6c3e1b7a2d4e58 allkeys allchannels +@all
user app on >4a71c0d2e9b35f86 ~app:* %R~shared:* &app.events +@all -@dangerous -@admin
user reader on #5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8 ~* -@all +@read +@connection
user metrics on >0d5b8e2f6a9c1347 -@all +info +dbsize
```

One user per line, `user <name>` followed by rules applied from left to right. A user starts disabled, without a
password and without permissions.

| Rule | Meaning |
|---|---|
| `on`, `off` | enable or disable the user |
| `>password`, `<password` | add or remove a password, written in clear text in the file; the server keeps only its SHA-256 hash, which `ACL LIST` shows |
| `#hash`, `!hash` | add or remove a password by its SHA-256 hash (64 hexadecimal digits), for files that must not contain passwords: `printf '%s' 'the password' \| sha256sum` |
| `nopass`, `resetpass` | accept any password / remove all passwords |
| `~pattern` | keys matching the glob pattern (`*`, `?`, `[...]`, `\`) may be read and written |
| `%R~pattern`, `%W~pattern`, `%RW~pattern` | read only, write only, both |
| `allkeys`, `resetkeys` | `~*` / remove all key patterns |
| `&pattern`, `allchannels`, `resetchannels` | pub/sub channels the user may use |
| `+@category`, `-@category` | allow or deny all commands of a category; `@all` is every command |
| `+command`, `-command` | allow or deny one RESP command |
| `allcommands`, `nocommands` | `+@all` / `-@all` |
| `reset` | `off resetpass resetkeys resetchannels nocommands` |

Rules are applied in order: `+@all -@dangerous` allows everything except the dangerous commands, while `-@dangerous
+@all` allows everything. Categories (`ACL CAT` lists them, `ACL CAT write` the commands of one):

| Category | Commands |
|---|---|
| `read` | reading values: `GET`, `MGET`, `STRLEN`, `EXISTS`, `TTL`; memcached `get`, `gets`, `mg`; native `GET`, `GETM`, `MGET`, `HAS` |
| `write` | changing values: `SET`, `DEL`, `INCR`, `EXPIRE`, `APPEND`; memcached storage, `delete`, `incr`, `touch`, `ms`, `md`, `ma`; native stores, `DEL`, `INCR`, `TOUCH` |
| `keyspace` | commands about keys rather than values: `DEL`, `EXISTS`, `EXPIRE`, `KEYS`, `SCAN`, `DBSIZE`, `TYPE` |
| `admin` | `SAVE`, `BGSAVE`, `BGREWRITEAOF`, `CONFIG`, `ACL`, `REPLICAOF`, `CLUSTER` |
| `dangerous` | `FLUSHALL`, `FLUSHDB`, `KEYS`, `INFO`, `CONFIG`, `ACL`, the persistence commands; memcached `flush_all`, `stats`; native `FLUSH`, `STATS` |
| `pubsub` | `SUBSCRIBE`, `PUBLISH` and the other pub/sub commands |
| `transaction` | `MULTI`, `EXEC`, `DISCARD`, `WATCH`, `UNWATCH` |
| `connection` | `AUTH`, `HELLO`, `PING`, `ECHO`, `QUIT`, `SELECT`, `CLIENT`, `COMMAND`, `TIME`; always allowed, so that a client can authenticate and leave |
| `replication` | the replication link of a replica (native `REPLSYNC`) |

`+command` and `-command` name RESP commands; memcached and native commands follow the categories. Every key a
command names must match a key pattern of the user for the kind of access: reading commands need read access,
writing commands write access, and commands that read and change a value (`INCR`, `GETSET`, `GETDEL`, memcached
`incr`, `gat`) both. `KEYS` and `SCAN` take a pattern rather than key names and are governed by their categories
only.

When the file defines a user `default` that is `on` with `nopass`, connections start logged in as that user;
otherwise every connection must authenticate first. A file without users, an unknown rule or category, or a user
defined twice stops the start with the file name and line number:

```
[error] access control list: /etc/arenakv/users.acl: line 3: '+@writes': unknown category
```

### Authenticating

| Protocol | How a client authenticates | Before authentication | Denied command |
|---|---|---|---|
| RESP | `AUTH password` (user `default`), `AUTH user password`, `HELLO 2 AUTH user password`; `redis-cli --user app --pass ...` | `-NOAUTH Authentication required.` | `-NOPERM User app has no permissions to run the 'flushall' command`, `-NOPERM No permissions to access a key` |
| memcached | first command `set <any key> 0 0 <n>` with the data `<user> <password>`, answered `STORED` (the scheme of memcached `-Y`) | `CLIENT_ERROR unauthenticated` | `CLIENT_ERROR permission denied` |
| native | `AKVP_AUTH` with `user:password` or a password; `akv_cli --auth app:...`, `akvc_auth()` | status `AKVP_NOAUTH` | status `AKVP_NOPERM` |

A wrong password answers `-WRONGPASS invalid username-password pair or user is disabled.` (RESP), `CLIENT_ERROR
authentication failure` (memcached) or `AKVP_NOAUTH`, and counts in `auth_errors`. The hash of the given password is
compared with every stored hash of the user without stopping at the first match. Credentials travel in clear text
unless the connection uses TLS.

### Inspecting and reloading

| Command | Result |
|---|---|
| `ACL WHOAMI` | the user of the connection |
| `ACL USERS` | all user names |
| `ACL LIST` | every user in canonical form: `user app on #4f1d... ~app:* %R~shared:* &app.events +@all -@dangerous -@admin` |
| `ACL GETUSER name` | flags, password hashes, command rules, key patterns and channels of one user |
| `ACL CAT [category]` | the categories, or the RESP commands of one |
| `ACL DRYRUN user command [arg ...]` | `OK` when the user may run the command, or the reason it may not |
| `ACL LOAD` | reads the file again; a file with an error is refused and the previous rules stay in force |

After `ACL LOAD`, connections whose user was removed or disabled are closed; other connections continue with the
new rules of their user. `ACL SETUSER`, `ACL DELUSER` and `ACL SAVE` are not provided: the file is the only source
of users, so it can be managed with the rest of the configuration.

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

The certificate and key are read at start; a renewed certificate takes effect after a restart.

### Encrypted native protocol

`--native-tls` encrypts the native listener and the replication links, over TCP and over RDMA (rdmaduct encrypted
endpoints: a TLS 1.3 handshake, then AES-256-GCM per message; [`../../rdmaduct/README.md`](../../rdmaduct/README.md#encryption)).
It uses the same `--tls-cert`, `--tls-key` and `--tls-ca` as the TLS listeners and needs a `TLS=1` build. With
`--tls-ca`, native clients must present a certificate signed by a CA in the file, and a replica verifies its
primary's certificate against it and against the host given to `--replicaof`; a replica without `--tls-ca` logs a
warning, because its link is then encrypted but the primary is not verified. Every node of a replication group
must use the same setting: an unencrypted peer is disconnected. Each message costs 17 bytes of the message limit.

The session below uses the certificates from above, a primary on port 17600 and a replica on 17601 that requires
client certificates (`arenakvd --port 17600 --native-tls --tls-cert server.crt --tls-key server.key` and
`arenakvd --port 17601 --native-tls --tls-cert server.crt --tls-key server.key --tls-ca ca.crt --replicaof
127.0.0.1:17600`):

```
$ akv_cli --port 17600 --tls --cacert ca.crt set t1 secure
$ akv_cli --port 17600 --tls --cacert ca.crt --sni cache.example.net get t1
secure
$ akv_cli --port 17600 --tls --cacert ca.crt --sni other.example.net get t1
akv_cli: cannot connect to 127.0.0.1:17600: protocol violation
$ akv_cli --port 17600 get t1
akv_cli: connection closed by peer
$ akv_cli --port 17601 --tls --cacert ca.crt --cert server.crt --key server.key get t1
secure
```

The C client takes the same settings in `struct duct_opts` (`tls`, `tls_ca`, `tls_cert`, `tls_key`,
`tls_server_name`), and `kvpace --driver native --tls --cacert ca.crt` measures the encrypted path. Registered-memory
and one-sided rdmaduct operations are not available on encrypted endpoints; the native protocol uses neither.

## Network exposure

- Bind the text listeners to the addresses clients use (`--text-addr`) and the native listener with `--addr`; the
  defaults listen on all IPv4 addresses.
- Expose the plain listeners only to trusted networks, use `--auth-file` everywhere, and TLS (and `--native-tls`)
  where traffic crosses untrusted links. Over RDMA each connection exposes a 4 KiB flow-control page to the peer with remote-write access
  ([`../../SECURITY.md`](../../SECURITY.md)); do not accept RDMA connections from untrusted hosts.
- There is no rate limiting; `--max-conns` and `--idle-timeout` bound the connections.

## Persistence

### Choosing a mechanism

| Mechanism | Option | What survives | What can be lost | Cost while running |
|---|---|---|---|---|
| none | | nothing | everything at a stop or crash | none |
| snapshot | `--snapshot FILE` | the content at the last save | changes since the last `SAVE`, `BGSAVE`, periodic save or clean stop | a copy of the whole content per save |
| append-only log | `--appendonly DIR` | every change written to the log | `always`: nothing that was acknowledged; `everysec`: up to about one second (tens of milliseconds when only the process is killed); `no`: what the kernel had not written | one record per change, and the fsync policy |
| warm restart | `--warm-restart FILE` | the cache memory across a stop and start of the process (upgrade, configuration change) | everything after a crash or a reboot (the memory is then not trusted) | none (the memory is a file on tmpfs) |

The mechanisms combine. At start the server takes the warm-restart memory when it is valid, otherwise the
append-only log, otherwise the snapshot, and logs which one it used. A typical cache that must keep its content over
upgrades uses `--warm-restart`; one that must also survive crashes adds `--appendonly`.

### Snapshots

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
- With a valid warm-restart memory or an append-only log, the snapshot is not loaded (`snapshot not loaded: the
  append-only log is newer`).

### Append-only log

- `--appendonly DIR` records every change: stores of all kinds, increments, deletes, expiry changes, flushes, and
  removals by expiry and eviction. Each record holds the resulting state (the full value and the absolute expiry
  time), so replaying a record twice changes nothing.
- The directory holds `manifest`, one `base-N.aof` and one or more `incr-N.aof` files. A new directory gets a base
  file with the current content (for example the content loaded from a snapshot).
- fsync policies, with the semantics of Valkey's `appendfsync`:

  | `--appendfsync` | A reply to a change is sent | Lost when the process is killed | Lost in a crash of the machine |
  |---|---|---|---|
  | `always` | after the change is on disk; changes of many connections share one `fdatasync` | nothing that was acknowledged | nothing that was acknowledged |
  | `everysec` (default) | at once; the log is written every 10 ms and synced about once per second | the changes not yet written: the last 10 ms, plus a batch being written and scheduling delays | up to about one second of changes |
  | `no` | at once; the log is written every 10 ms, the kernel syncs when it decides | as with `everysec` | what the kernel had not yet written |

  The log writer collects changes for 10 ms (or 1 MiB) before it writes them, instead of waking for every change:
  a wake-up per change costs a context switch per write, which cost a server on one CPU a third of its throughput under 50 % writes.
  Valkey writes its log before it replies, so a killed Valkey process loses nothing with `everysec`.
  `tests/durability.py` kills a writing server with `SIGKILL` and checks that with `always` nothing acknowledged is
  lost, and with `everysec` nothing acknowledged earlier than 0.25 s before the kill; it prints the number lost.
- Rewriting: `BGREWRITEAOF`, or automatically when the log grew by `--aof-rewrite-percentage` of its base and is at
  least `--aof-rewrite-min-size`. The server switches to a new increment file, writes a new base from the current
  content in the background, then replaces the manifest and removes the old files. Changes continue during the
  rewrite; a crash at any point leaves a manifest whose files replay to the latest state.
- Loading: the start-up log reports `append-only log loaded: N records from M files`. Transactions are applied
  completely or not at all; an unfinished transaction at the end of the log is not applied and reported.
- INFO reports `aof_enabled`, the sizes, the rewrite and write status and the number of syncs
  ([RESP INFO fields](#resp-info-fields)). `aof_last_write_status:err` means the disk refused a write (full, read
  only); the server retries every second and, with `always`, holds the replies until a write succeeds.

### Warm restart

- `--warm-restart FILE` places the cache memory in FILE, which must be on tmpfs (normally `/dev/shm`; the start
  fails when the directory is not writable). The file has the size of `--capacity` plus a header of a few KiB.
- A stop by `SIGTERM`, `SIGINT` or `SIGUSR1` records the log positions and the time in the file. The next start with
  the same `--capacity`, `--shards` and `--max-entry` rebuilds the hash index from the memory instead of starting
  empty: `warm restart: N entries restored in S s (M expired while stopped)`. Expiry times count the time the server
  was stopped.
- The memory is discarded, with a warning, when the previous process did not stop cleanly (crash, `SIGKILL`), when
  one of these options changed, or when the file is damaged or written by an incompatible version. The server then
  starts from the append-only log or the snapshot, if configured.
- The file does not survive a reboot (tmpfs). To start empty on purpose, remove the file before starting or run
  `FLUSHALL` before stopping.
- Memory accounting: the pages of the file count as shared memory (`Shmem` in `/proc/meminfo`) rather than as
  anonymous memory of the process, and a container's tmpfs size limit applies to them.

### Backup and restore

```
redis-cli -p 6379 BGSAVE                          # or SAVE; memcached clients: none, use the RESP listener
redis-cli -p 6379 LASTSAVE                        # changes when the background save finished
cp /var/lib/arenakv/arenakv.snap /backup/arenakv-$(date +%F).snap
```

To restore: stop the server (it writes its own snapshot at stop), replace the snapshot file, start the server. To
start empty, move the file away before starting. To discard the cache contents at the next stop, start with
`--no-save-on-stop` or remove the file after stopping.

An append-only log directory is copied with `cp -a` while `aof_rewrite_in_progress` is 0. A copy taken while
changes are written may end in a partial block; that block is cut off when the copy is loaded, so the copy restores
the changes up to that point. To restore, stop the server, replace the directory, and start it.

### Damaged files

Each block of records carries a CRC32C; a damaged or truncated snapshot is refused and the server does not start:

```
[error] snapshot /var/lib/arenakv/arenakv.snap cannot be loaded (Bad message); move it away to start empty
cannot start server: Bad message
```

Move the file away (keep it for analysis) and start again; the server starts empty.

In the append-only log, an incomplete last block of the last increment file (the remains of a crash during a
write, including a zero-filled tail) is removed at start with a warning (`... bytes of an incomplete last write
removed`), as Valkey does with `aof-load-truncated yes`. A damaged block anywhere else stops the start:

```
[error] append-only log: damaged block at offset 1048600 of /var/lib/arenakv/aof/incr-7.aof
[error] append-only log in /var/lib/arenakv/aof cannot be opened (Bad message); see the operations guide for damaged logs
```

Keep a copy of the directory, then either restore a backup or start once with `--aof-load-damaged`: the server
applies the records before the damaged block, skips the rest, and writes a new base at once, so the log is whole
again. A directory with log files but no `manifest` is refused as well.

### Format

Snapshots, little-endian. Header: magic `AKVSNAP1` (8 bytes), version 1 (4), reserved (4), creation time in Unix
seconds (8). Blocks: magic `BLK1` (4), record count (4), payload length (4), CRC32C of the payload (4), payload of
records (key length 2, reserved 2, client flags 4, value length 4, remaining time to live in ms 8 with -1 for none,
key, value). An end block with record count 0 holds the total number of records (8). Definition: `src/srv.h`,
`src/snapshot.c`.

Append-only log files, little-endian. Header: magic `AKVLOG01` (8), version 1 (4), file kind 0 base / 1 increment
(4), creation time (8). Blocks: magic `ALG1` (4), record count (4), payload length (4), CRC32C (4), records (kind 1,
reason 1, key length 2, client flags 4, value length 4, absolute expiry in Unix ms 8 with 0 for none, transaction id
8, key, value). Record kinds: 1 store, 2 delete, 3 expiry change, 4 flush, 100 end of a transaction. Definition:
`src/aof.h`, `src/aof.c`. Warm-restart file: `src/akv.c` (header `struct warm_hdr`).

## Replication

### Setting up a replica

```
arenakvd --transport tcp --addr 10.0.0.1 --port 7600 --resp-port 6379 ...                    # primary
arenakvd --transport tcp --addr 10.0.0.2 --port 7600 --resp-port 6379 --replicaof 10.0.0.1:7600 ... # replica
```

A replica connects to the native listener of its primary (`--replicaof HOST:PORT`), over the transport of its own
`--transport` option, so replication runs over RDMA where both hosts have a device. With access control on the
primary, `--primary-auth USER:PASSWORD` names a user with the `replication` category (`+@replication`). `REPLICAOF
host port` starts or changes replication at run time, and `REPLICAOF NO ONE` stops it.

### How it works

- The primary keeps every change (stores, deletes, expiry changes, flushes, removals by expiry and eviction) in a
  backlog of `--repl-backlog-size` bytes (default 64 MiB), addressed by a byte offset under a replication id. The
  backlog starts recording when the first replica connects or the server follows a primary, so a server without
  replicas does no replication work.
- A replica asks for the stream from its replication id and offset. When the primary still holds that offset it
  continues from there (`replica ...: continuing at offset N` in the primary's log); otherwise it sends a full copy
  of its content, then the changes from the offset at which the copy started (`full copy, stream from offset N`).
  A replica that restarts has no offset and receives a full copy.
- Replication is asynchronous: the primary answers clients before the replicas have the change. `WAIT numreplicas
  timeout-ms` returns how many replicas acknowledged the last change made through the connection's worker, after at
  most the timeout (0 waits without limit).
- A replica applies the changes in order, transactions completely or not at all, and refuses client writes
  (`READONLY You can't write against a read only replica.`, memcached `CLIENT_ERROR read only replica`, native
  `AKVP_READONLY`) unless started with `--replica-read-only no`.
- Expiry: the primary decides. A replica hides entries whose expiry passed but removes them only when the primary's
  delete arrives, so a change that the primary made before the expiry still applies. Expiry times are absolute, so
  the clocks of the hosts must agree (NTP); an offset between clocks shifts when entries disappear from the replica.
- A replica's own eviction is local: a replica with less `--capacity` than its primary evicts earlier and then
  holds fewer entries.
- A replica keeps the stream in its own backlog, so other replicas can follow it (chains) and, after a promotion,
  the replicas of the old primary can continue from their offset.

### Failover

**Automatic failover** is done by `akv_sentinel` monitors, which follow the procedure of Redis Sentinel. Run at
least three, on hosts that fail independently of each other and of the nodes; every monitor lists all nodes of the
group in the same order (the first is the primary until a failover) and the other monitors:

```
akv_sentinel --name cache --listen 26379 --state /var/lib/arenakv/sentinel.state \
    --node 10.0.0.1:6379:7600 --node 10.0.0.2:6379:7600 --node 10.0.0.3:6379:7600 \
    --peer 10.0.0.5:26379 --peer 10.0.0.6:26379 --down-after 5000 --failover-timeout 60000
```

`--node HOST:RESP_PORT:NATIVE_PORT` gives the address used both by the monitors and by replicas, the RESP port for
monitoring and commands, and the native port that `REPLICAOF` names. With access control on the nodes,
`--node-auth USER:PASSWORD` names a user with the `admin` category (`REPLICAOF`). `--state` keeps the epochs, the
vote and the current primary; without it a restarted monitor forgets them.

1. Each monitor pings the primary every 100 ms. A primary without a valid reply for `--down-after` milliseconds is
   down for that monitor (`+sdown`); with `--quorum` monitors agreeing (default: a majority) it is objectively down
   (`+odown`).
2. A monitor then asks for the votes of the others in a new epoch. Each monitor votes once per epoch, for the
   first requester, and stores the vote before answering. A monitor with the votes of a majority of all monitors
   leads the failover (`+elected-leader`); the others wait `--failover-timeout` before a new attempt.
3. The leader promotes the reachable replica with the highest replication offset (`REPLICAOF NO ONE`), waits until
   it reports the primary role, records it with the epoch (`+switch-master`), tells the other monitors and points
   the other replicas at it. They continue from their offsets.
4. Monitors exchange their configuration every second and adopt a newer one. A monitor that reaches a majority of
   the monitors points every node that does not follow the configured primary at it (`+convert-to-slave`,
   `+fix-slave-config`), so a former primary that comes back becomes a replica. Several monitors may send the same
   `REPLICAOF`; it is idempotent.

Clients ask a monitor for the primary: `SENTINEL GET-MASTER-ADDR-BY-NAME name` returns its address and RESP port,
and `SENTINEL MASTERS`, `SENTINEL REPLICAS name` and `SENTINEL SENTINELS name` return the state lists that Sentinel
clients read (redis-py's `Sentinel` works unchanged). `SENTINEL FAILOVER name` moves the primary of a healthy group
(it still needs a majority of votes); `SENTINEL CKQUORUM name` reports whether a majority is reachable. Events go
to standard error.

What a failover cannot do: changes that the old primary acknowledged but had not sent to the promoted replica are
lost (`WAIT` bounds that for the writes that need it); a primary cut off from the monitors but reachable by clients
keeps accepting writes until it is demoted, and those writes are discarded when it rejoins. Without a majority of
the monitors no failover happens. A monitor watches one replicated group; the Redis Cluster protocol's slot map
(below) is not changed by a failover. Monitors accept requests without authentication: bind them to a trusted
network.

**Manual failover**, without monitors:

1. Choose the replica with the highest `slave_repl_offset` (`INFO replication`).
2. Promote it: `REPLICAOF NO ONE`. It keeps the old replication id as its second id (`master_replid2`).
3. Point the other replicas at it: `REPLICAOF <new primary> <native port>`. They continue from their offsets
   without a full copy, as long as they did not get ahead of the promoted replica.
4. Point the clients at the new primary. Clients of the native protocol that use an `akvc_pool` entry
   `primary,replica,...` find it with `akvc_pool_reconnect()`; RESP clients follow their own configuration.

Changes that the old primary acknowledged but had not yet sent are lost; `WAIT` bounds that for the writes that
need it.

### Cluster protocol

`--cluster-config FILE --cluster-node-id ID` makes the RESP listener speak the Redis Cluster protocol with a slot
map from FILE (a copy on every node, or one file that the nodes of one host share):

```
# id  host      resp-port native-port role
node n1 10.0.0.1 6379 7600 primary 0-5460
node n2 10.0.0.2 6379 7600 primary 5461-10922
node n3 10.0.0.3 6379 7600 primary 10923-16383
node r1 10.0.0.4 6379 7600 replica-of n1
```

- Keys map to the 16384 slots of the Redis Cluster specification (CRC16, hash tags `{...}`). A command for a key of
  another node's slot answers `MOVED <slot> <host>:<port>`; keys of several slots in one command answer
  `CROSSSLOT`. Cluster clients (`valkey-cli -c`, redis-py `RedisCluster`, kvpace `--driver resp-cluster`) follow
  the redirections.
- A replica in the map follows its primary automatically (through the primary's native port) and serves reads of
  its primary's slots to connections that send `READONLY`; other commands are redirected to the primary.
- `CLUSTER SLOTS`, `SHARDS`, `NODES`, `INFO`, `MYID`, `KEYSLOT`, `COUNTKEYSINSLOT` and `GETKEYSINSLOT` describe
  the map. There is no gossip and no automatic failover of slot owners. The map changes when the file is edited and
  `CLUSTER RELOAD` is sent to every node, or slot by slot during resharding (below).

#### Resharding

`akv_reshard` moves slots with their keys from one primary to another while clients keep working:

```
akv_reshard --from 10.0.0.1:6379 --to 10.0.0.2:6379 --slots 0-999,4000 [--batch 100] [--timeout 5000] \
    [--auth USER:PASSWORD]
```

It follows the Redis Cluster procedure ("Live reconfiguration"), and the same commands can be sent by hand:

1. `CLUSTER SETSLOT <slot> IMPORTING <source id>` on the target and `CLUSTER SETSLOT <slot> MIGRATING <target id>`
   on the source, for every slot. From then on the source serves keys of the slot that it still holds, answers
   `ASK <slot> <target>` for keys it does not hold (and `TRYAGAIN` for a command whose keys are partly moved), and
   the target serves the slot to a connection that sent `ASKING` just before. Cluster clients (redis-py
   `RedisCluster`, kvpace `--driver resp-cluster`) follow `ASK`.
2. `CLUSTER GETKEYSINSLOT <slot> <count>` on the source lists keys, and `MIGRATE <target host> <port> "" 0 <timeout>
   REPLACE KEYS <key> ...` moves them: the source copies them with `RESTORE-ASKING` and deletes them. While a batch
   is copied, the engine shards of its keys stay locked on the source, so no command sees or changes a key between
   its copy and its deletion; commands on a slot being moved wait for that, at most the `MIGRATE` timeout.
3. `CLUSTER SETSLOT <slot> NODE <target id>` on the target, the source and every other node (replicas included)
   makes the target the owner; the source refuses while it still holds keys of the slot. A primary also passes the
   new owner to its replicas in the replication stream, so sending the command to the primaries only is enough.
   Each node then rewrites its map file; `CLUSTER SAVECONFIG` forces it to disk (fsync), which `akv_reshard` sends
   at the end.

`valkey-cli --cluster reshard` (Valkey 9) runs the same procedure and works against arenakv nodes; it informs
primaries only and relies on the replication stream for replicas. `valkey-cli --cluster` commands that change the
membership (`add-node`, `del-node`, `create`) need `CLUSTER MEET` and gossip, which arenakv does not have.

An interrupted `akv_reshard` is completed by running the same command again. `CLUSTER SETSLOT <slot> STABLE` ends a
move without changing the owner. The first listing of a slot's keys scans the engine once for every slot being
moved, so marking all slots before moving keys (as `akv_reshard` does) costs one scan. `MIGRATE`, `DUMP` and
`RESTORE` belong to the `dangerous` category; `DUMP` and `RESTORE` use arenakv's own value format (with a CRC32C), so
they exchange values only between arenakv nodes. Map files are rewritten on every ownership change without fsync,
which a process crash does not lose; after an operating-system crash between the change and `CLUSTER SAVECONFIG`
a node can start with an older map.
- The memcached and native listeners do not check slots; their clients distribute keys themselves (`akvc_pool`).

### Monitoring replication

`INFO replication` reports `role`, `connected_slaves` with one `slaveN:ip=...,port=...,state=...,offset=...,lag=...`
line per replica (`lag` in bytes of stream not yet acknowledged), `master_replid`, `master_replid2`,
`master_repl_offset`, `second_repl_offset` and the backlog (`repl_backlog_size`, `repl_backlog_first_byte_offset`,
`repl_backlog_histlen`); on a replica also `master_host`, `master_port`, `master_link_status` (`up` once the copy
is complete), `master_last_io_seconds_ago` and `slave_repl_offset`. `ROLE` gives the same in the form of Redis.

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
| `aof_enabled`, `aof_rewrite_in_progress`, `aof_last_bgrewrite_status`, `aof_last_write_status` | append-only log configured, rewrite running, `ok`/`err` of the last rewrite and of the last write |
| `aof_current_size`, `aof_base_size`, `aof_buffer_length` | bytes of all log files, of the base file, and of changes not yet written |
| `aof_rewrites`, `aof_fsyncs` | completed rewrites and `fdatasync` calls since the start |
| `warm_restart`, `warm_restart_entries`, `warm_restart_rebuild_us` | outcome of the warm restart at start (`off`, `created`, `restored`, `mismatch`, `unclean`, `damaged`), entries restored, time to rebuild the index |
| `total_connections_received`, `rejected_connections` | accepted connections; connections closed by `--max-conns` |
| `total_commands_processed` | requests of all protocols |
| `keyspace_hits`, `keyspace_misses` | reads of all protocols that found / did not find the key |
| `evicted_keys` | entries removed to make room (including removals from a full index window) |
| `expired_keys` | entries removed because their expiry passed |
| `pubsub_channels`, `pubsub_patterns` | channels with subscribers, distinct subscribed patterns |
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
| Restart without losing data | `--warm-restart` keeps the memory over a stop and start; `--appendonly` or `--snapshot` also cover crashes ([Choosing a mechanism](#choosing-a-mechanism)) |
| Change capacity, threads, ports or credentials | edit the configuration, restart; the snapshot is loaded into the new capacity |
| Empty the cache | RESP `FLUSHALL`, memcached `flush_all`, `akv_cli flush`; with a snapshot configured, the next save writes the empty state |
| Upgrade | stop (log synced, snapshot written, warm memory kept), install the new binary, start; a version that cannot read the warm-restart memory discards it and loads the log or snapshot; snapshot format version 1 and log format version 1 are read by every version that writes them |
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
| `snapshot directory ... is not writable` (or `warm-restart`, `append-only log`) | wrong path or permissions | create the directory, give it to the server user |
| `append-only log ... cannot be opened (Bad message)` | a damaged block before the end of the log, or log files without a manifest | [Damaged files](#damaged-files) |
| `warm restart: memory discarded (...)` | crash, changed `--capacity`/`--shards`/`--max-entry`, or another version | expected after a crash; the log or snapshot is loaded instead |
| `aof_last_write_status:err` | the disk refuses writes (full, read-only) | free space; with `always`, clients wait until writes succeed again |
| `master_link_status:down` on a replica | the primary is unreachable, or refused the credentials | the replica's log names the reason; it reconnects every second |
| `replica ...: more than the backlog size behind` | the replica could not keep up (slow network, paused) | it resynchronizes fully; raise `--repl-backlog-size` |
| `MOVED` / `CROSSSLOT` replies | cluster mode: the key belongs to another node, or the command names keys of several slots | use a cluster client; group keys with hash tags `{...}` |
| `TLS options need a build with TLS=1` | TLS listener options on a build without TLS | build with `make TLS=1` |
| clients get `NOAUTH` / `CLIENT_ERROR unauthenticated` / `AKVP_NOAUTH` | `--aclfile` or `--auth-file` is set | authenticate first ([Authentication](#authentication-and-access-control)) |
| `NOPERM ...` / `CLIENT_ERROR permission denied` / `AKVP_NOPERM` | the user's rules do not allow the command or key | `ACL DRYRUN user command args` names the reason; change the ACL file and run `ACL LOAD` |
| `access control list: FILE: line N: ...` at start | an error in the ACL file | the message names the rule; `--auth-file` and `--aclfile` together are refused as well |
| `SERVER_ERROR object too large for cache`, `ERR string exceeds maximum allowed size`, `AKVP_TOOBIG` | entry above `min(max-entry, capacity / shards / 2)`, or a native message above `--max-msg` | raise `--max-entry` or `--max-msg`, or lower `--shards` |
| `SERVER_ERROR out of memory storing object`, `OOM ...`, `AKVP_NOMEM` | full cache with `--no-evict` | delete entries, raise `--capacity`, or allow eviction |
| hit ratio falls although keys were stored | eviction (see `evictions`) or expiry (`expired`) | raise `--capacity` or the times to live |
| `rejected_connections` grows | `--max-conns` reached | raise the limit, check clients for connection leaks |

## Compatibility notes

- RESP: string commands only; no lists, hashes, sets, streams, scripting or keyspace notifications. RESP2 and
  RESP3 (`HELLO 3`); transactions and pub/sub as in Valkey, with two differences: a command whose arguments are
  invalid is reported inside the `EXEC` reply rather than aborting the transaction when it is queued (unknown and
  refused commands do abort it), and `WATCH` compares CAS versions, so a key created and removed again between
  `WATCH` and `EXEC` does not abort the transaction. Sharded pub/sub (`SSUBSCRIBE`) is not provided.
- memcached: no binary protocol, no delayed `flush_all`, no meta flags `b`, `I`, `E` and no `N` on `mg`. `version`
  reports `1.6.0-arenakv-<version>` (the protocol level of the meta commands).
- Verified clients (`make compat`, needs Docker): valkey-cli, valkey-benchmark, memtier_benchmark (RESP and memcached
  text), redis-py.
