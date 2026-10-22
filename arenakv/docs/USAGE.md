# Running and querying arenakv

This guide covers building and installing `arenakvd`, starting it, and reading and writing data through each of
its three protocols. Administration (security, persistence, monitoring, deployment, troubleshooting) is in
[`OPERATIONS.md`](OPERATIONS.md); the native wire format is in [`PROTOCOL.md`](PROTOCOL.md).

The example outputs in this guide were produced by the commands shown, against a server built from this
repository. The sessions in sections 4 to 6 used a test server started with
`arenakvd --transport tcp --port 17600 --resp-port 16379 --memcached-port 16211 --text-addr 127.0.0.1 --capacity 512M`;
the RDMA example used a second server with `--transport rdma`. Numbers in the outputs (CAS versions, times to live,
operation counts) differ from run to run.

## Contents

1. [Build and install](#1-build-and-install)
2. [Quick start](#2-quick-start)
3. [Starting the server](#3-starting-the-server)
4. [Querying with the Redis protocol (RESP2)](#4-querying-with-the-redis-protocol-resp2)
5. [Querying with the memcached protocol](#5-querying-with-the-memcached-protocol)
6. [Querying with the native protocol](#6-querying-with-the-native-protocol)
7. [Limits of keys and values](#7-limits-of-keys-and-values)

## 1. Build and install

Requirements: Linux, gcc 13 or newer (or clang 18 or newer), `make`. RDMA support needs `libibverbs-dev` and
`librdmacm-dev`; TLS needs `libssl-dev`.

```
make -C arenakv                         # build/<arch>/arenakvd, akv_cli, akv_bench, libarenakv.a
make -C arenakv TLS=1                   # with TLS listeners: build/<arch>-tls/
make -C arenakv RDMA=0                  # without the RDMA libraries: build/<arch>-tcp/
make -C arenakv test                    # test suites (see the project README)
sudo make -C arenakv install            # /usr/local/bin/{arenakvd,akv_cli,akv_bench}
make -C arenakv install PREFIX=$HOME/.local DESTDIR=/tmp/pkg   # other locations, staging for packages
```

`install` installs the variant selected by the same `TLS=` and `RDMA=` arguments as the build.

| Program | Purpose |
|---|---|
| `arenakvd` | the server |
| `akv_cli` | command-line client for the native protocol, over TCP or RDMA |
| `akv_bench` | engine micro-benchmarks (used by the benchmark suite) |

`arenakvd --version` prints the server version and the source revision it was built from; `arenakvd --help`
lists every option.

## 2. Quick start

Start a server with all three protocols on the local host:

```
arenakvd --capacity 1G --text-addr 127.0.0.1 --resp-port 6379 --memcached-port 11211 --transport tcp --port 7600
```

The server logs one line when it is ready:

```
2026-10-21T17:07:08Z [info] arenakvd listening: native 7600, memcached 11211, resp 6379, tls memcached 0, tls resp 0; 1024 MiB in 64 shards; tag matching: sse2
```

Then, from another shell:

```
$ redis-cli -p 6379 SET block-17 payload EX 3600         # valkey-cli works the same way
OK
$ printf 'get block-17\r\n' | nc -q1 127.0.0.1 11211
VALUE block-17 0 7
payload
END
$ akv_cli --port 7600 get block-17
payload
```

All three protocols reach the same data: a key stored through one is visible through the others.

## 3. Starting the server

### Listeners

`arenakvd` opens up to five listeners. At least one must be configured.

| Listener | Option | Default | Address option |
|---|---|---|---|
| native protocol over rdmaduct (TCP or RDMA) | `--port PORT\|off` | 7600 | `--addr` (default: all IPv4 addresses) |
| memcached text and meta protocol | `--memcached-port PORT\|off` | off | `--text-addr` (default: all IPv4 addresses) |
| RESP2 (Redis protocol) | `--resp-port PORT\|off` | off | `--text-addr` |
| memcached over TLS | `--tls-memcached-port PORT\|off` | off | `--text-addr` |
| RESP2 over TLS | `--tls-resp-port PORT\|off` | off | `--text-addr` |

Port 0 lets the kernel choose a free port; the chosen ports are printed in the start-up line. The listeners accept
IPv4 only.

**Transport of the native listener.** `--transport auto` (the default) listens with RDMA when the host has an RDMA
device and with TCP otherwise; `tcp` and `rdma` force one. The RDMA connection manager has a port space of its own,
separate from TCP: an RDMA listener on port 7600 and a TCP listener on port 7600 can exist at the same time, and a
client must use the same transport as the server. Over RDMA, `--addr` must be the IPv4 address of the network
interface that belongs to the RDMA device (`rdma link show` lists the pairs). `--wait event` (default) lets
worker threads sleep on completion events; `--wait poll` spins and pays off only with a dedicated core per
connection.

### All options

Sizes accept the suffixes `K`, `M` and `G` (powers of 1024). Invalid values are rejected at start with a message
that names the option and the accepted range.

| Option | Default | Accepted values and meaning |
|---|---|---|
| `--transport tcp\|rdma\|auto` | auto | transport of the native listener |
| `--wait poll\|event` | event | waiting strategy of RDMA connections |
| `--addr ADDRESS` | all IPv4 | address of the native listener |
| `--port PORT\|off` | 7600 | native listener |
| `--max-msg BYTES` | 65536 | 64 to 1073741824: largest native message; a request carries key + value + up to 36 bytes of header |
| `--depth N` | 16 | 2 to 65535: receive and send buffers per native connection |
| `--text-addr ADDRESS` | all IPv4 | address of the memcached and RESP listeners |
| `--memcached-port`, `--resp-port` | off | text listeners |
| `--tls-memcached-port`, `--tls-resp-port` | off | TLS listeners; need `--tls-cert` and `--tls-key` and a `TLS=1` build |
| `--tls-cert FILE`, `--tls-key FILE` | | server certificate chain and private key, PEM |
| `--tls-ca FILE` | | require client certificates signed by a CA in FILE |
| `--native-tls` | off | encrypt the native listener and replication links with the same files ([OPERATIONS](OPERATIONS.md#encrypted-native-protocol)) |
| `--capacity SIZE` | 256M | cache memory; at least 4 KiB per shard |
| `--shards N` | 64 | power of two from 1 to 1024 |
| `--max-entry SIZE` | 1M | largest key + value; also limited to half of a shard's memory (`capacity / shards / 2`) |
| `--avg-entry BYTES` | 0 | presize the hash index for this mean entry size; 0 grows it on demand |
| `--no-evict` | off | refuse stores when memory is full instead of evicting |
| `--huge-pages` | off | advise transparent huge pages for the cache memory |
| `--threads N` | CPUs of the affinity mask | 1 to 4096 worker threads |
| `--max-conns N` | 0 (unlimited) | connections above the limit are closed after accepting |
| `--idle-timeout SECONDS` | 0 (never) | close connections without traffic |
| `--auth-file FILE` | | credentials (`user:password` per line); every protocol then requires authentication and every user has all rights ([OPERATIONS.md](OPERATIONS.md#authentication-and-access-control)) |
| `--aclfile FILE` | | users with passwords and per-user rights in a subset of the Valkey ACL file syntax; reloaded by `ACL LOAD`; excludes `--auth-file` |
| `--snapshot FILE` | | load at start, write on `SAVE`/`BGSAVE`, periodically and at stop; the directory must be writable |
| `--save-interval SECONDS` | 0 (off) | periodic snapshot when data changed |
| `--no-save-on-stop` | | skip the snapshot at stop |
| `--appendonly DIR` | | append-only log of every change in DIR (created with mode 0700); loaded at start ([OPERATIONS.md](OPERATIONS.md#append-only-log)) |
| `--appendfsync always\|everysec\|no` | everysec | when the log is synced to disk; with `always` a reply that follows a change is sent after the sync |
| `--aof-rewrite-percentage N` | 100 | rewrite the log when it grew by N% of its base file; 0 disables automatic rewrites |
| `--aof-rewrite-min-size SIZE` | 64M | ... and the log is at least SIZE |
| `--aof-load-damaged` | off | start with the records before a damaged block instead of refusing to start |
| `--warm-restart FILE` | | keep the cache memory in FILE (on tmpfs, normally under `/dev/shm`) so that a restart of the process keeps the entries ([OPERATIONS.md](OPERATIONS.md#warm-restart)) |
| `--replicaof HOST:PORT` | | follow the primary whose native listener is HOST:PORT ([OPERATIONS.md](OPERATIONS.md#replication)) |
| `--primary-auth USER:PASSWORD` | | credentials of a user with the `replication` category on the primary |
| `--replica-read-only yes\|no` | yes | refuse client writes while following a primary |
| `--repl-backlog-size SIZE` | 64M | changes kept for replicas that reconnect; at least 4K |
| `--cluster-config FILE`, `--cluster-node-id ID` | | Redis Cluster protocol with the slot map in FILE ([OPERATIONS.md](OPERATIONS.md#cluster-protocol)) |
| `--pubsub-output-limit SIZE` | 32M | unread messages of a subscriber before it is disconnected (the pubsub client-output-buffer-limit of Valkey) |
| `--log-level error\|warn\|info\|debug` | info | |
| `--log-file FILE` | standard error | reopened on `SIGHUP` |
| `--stats SECONDS` | 0 (off) | log a statistics line periodically |
| `--config FILE` | | read options from FILE |
| `-h`, `--help`; `-V`, `--version` | | print usage or version and exit |

### Configuration file

```
arenakvd --config /etc/arenakv/arenakvd.conf
```

One option per line, written as `name = value` or `name value`, with the long option name without dashes. A flag
stands alone (`no-evict`) or is written `no-evict = yes`; `no-evict = no` leaves it unset. `#` starts a comment.
Options given on the command line override the file, so a file can hold the defaults of a site and the command line
the per-instance values. An unknown name stops the start with `FILE:LINE: unknown option`. Example:
[`../contrib/arenakvd.conf`](../contrib/arenakvd.conf).

### Exit status and signals

| Exit status | Meaning |
|---|---|
| 0 | stopped by `SIGTERM`, `SIGINT` or `SIGUSR1` (after syncing the append-only log, the final snapshot and keeping the warm-restart memory, as configured), or `--help`/`--version` |
| 1 | the server could not start (port in use, snapshot or log damaged, TLS files unreadable, no RDMA device) |
| 2 | invalid options |

Start-up failures are logged with the listener or file concerned, for example:

```
2026-10-21T17:14:44Z [error] RESP listener on * port 6379: Address already in use
cannot start server: Address already in use
```

`SIGHUP` reopens the log file. The server runs in the foreground; use systemd or a container runtime to run it in
the background ([OPERATIONS.md](OPERATIONS.md#deployment)).

## 4. Querying with the Redis protocol (RESP2, RESP3)

Any Redis or Valkey client can be used: `redis-cli`, `valkey-cli`, and client libraries of any language. Requests
may be pipelined. A connection speaks RESP2 until it sends `HELLO 3`; RESP3 then returns nulls as `_`, maps for
`HELLO`, `CONFIG GET`, `PUBSUB NUMSUB` and `ACL GETUSER`, `INFO` as a verbatim string, and pub/sub messages as push
frames.

### Example session

```
127.0.0.1:16379> SET block-17 payload EX 3600
OK
127.0.0.1:16379> GET block-17
"payload"
127.0.0.1:16379> TTL block-17
(integer) 3599
127.0.0.1:16379> SET counter 10
OK
127.0.0.1:16379> INCRBY counter 5
(integer) 15
127.0.0.1:16379> MGET block-17 counter missing
1) "payload"
2) "15"
3) (nil)
127.0.0.1:16379> SET block-17 other NX
(nil)
127.0.0.1:16379> SCAN 0 MATCH block*
1) "0"
2) 1) "block-17"
127.0.0.1:16379> CONFIG GET maxmemory*
1) "maxmemory"
2) "536870912"
3) "maxmemory-policy"
4) "allkeys-lru"
127.0.0.1:16379> LPUSH list 1
(error) ERR unknown command 'LPUSH'
```

With redis-py:

```python
import redis

r = redis.Redis(host="127.0.0.1", port=16379)          # add password=..., username=... with --auth-file
r.set("session:42", b"\x00\x01payload", ex=600)       # values are binary-safe
print(r.get("session:42"), r.ttl("session:42"))      # b'\x00\x01payload' 600
print(r.incr("visits"), r.incr("visits", 10))        # 1 11
p = r.pipeline(transaction=False)                    # pipelining
for i in range(3):
    p.set(f"k{i}", i)
p.mget("k0", "k1", "k2")
print(p.execute())                                   # [True, True, True, [b'0', b'1', b'2']]
t = r.pipeline(transaction=True)                     # MULTI ... EXEC
t.incr("a")
t.incr("b")
print(t.execute())                                   # [1, 1]
ps = r.pubsub()
ps.subscribe("events")
r.publish("events", "hello")                         # 1 (one receiver)
```

### Transactions

`MULTI` starts a transaction; the following commands answer `QUEUED` and run on `EXEC`, which returns their replies
in an array. No other client reads or changes the keys of a transaction while it runs, and the append-only log
replays a transaction completely or not at all. An unknown command, or one the user's ACL refuses, while queueing
makes `EXEC` answer `EXECABORT` and run nothing. A command that fails while it runs (for example `INCR` of a
non-number) returns its error in its place, and the other commands run. `WATCH key ...` before `MULTI` makes `EXEC`
return a null array when another client changed one of the keys in between (or the key expired); `UNWATCH` and
`DISCARD` drop the watches.

```
$ valkey-cli -p 16379 SET balance 100
OK
$ printf 'WATCH balance\nMULTI\nDECRBY balance 30\nINCRBY spent 30\nEXEC\n' | valkey-cli -p 16379
OK
OK
QUEUED
QUEUED
1) (integer) 70
2) (integer) 30
```

### Publish and subscribe

`SUBSCRIBE channel ...` and `PSUBSCRIBE pattern ...` (glob patterns) deliver every `PUBLISH channel message` sent on
any connection; `PUBLISH` returns the number of receivers. A RESP2 connection with subscriptions accepts only the
pub/sub commands, `PING`, `QUIT` and `RESET`; a RESP3 connection accepts every command. A subscriber whose unread
messages exceed `--pubsub-output-limit` (default 32 MiB) is disconnected. Messages are not stored: a subscriber
receives only what is published while it is subscribed.

```
$ valkey-cli -p 16379 SUBSCRIBE events            # in one terminal
1) "subscribe"
2) "events"
3) (integer) 1
1) "message"
2) "events"
3) "block-17 evicted"

$ valkey-cli -p 16379 PUBLISH events "block-17 evicted"   # in another
(integer) 1
```

### Command reference

Command names are case-insensitive. Values and keys are byte strings; the engine has no other data types.

| Command | Reply | Notes |
|---|---|---|
| `GET key` | value or nil | |
| `SET key value [NX\|XX] [GET] [EX s\|PX ms\|EXAT unix-s\|PXAT unix-ms\|KEEPTTL]` | `OK`; nil when NX/XX prevented the store; with `GET` the old value | an expiry in the past deletes the key |
| `SETNX key value` | 1 stored, 0 present | |
| `SETEX key seconds value`, `PSETEX key ms value` | `OK` | |
| `GETSET key value` | old value or nil | |
| `GETDEL key` | value or nil | |
| `GETEX key [EX s\|PX ms\|EXAT\|PXAT\|PERSIST]` | value or nil | reads and changes the expiry |
| `MGET key [key ...]` | array | |
| `MSET key value [key value ...]` | `OK` | atomic: no reader sees some keys stored and others not |
| `APPEND key value` | new length | creates the key when absent |
| `STRLEN key` | length, 0 when absent | |
| `INCR`, `DECR key`; `INCRBY`, `DECRBY key delta` | new value | 64-bit signed; absent key counts as 0; errors for non-integers and overflow |
| `DEL key [key ...]`, `UNLINK ...` | number removed | |
| `EXISTS key [key ...]` | number present | |
| `TYPE key` | `string` or `none` | |
| `EXPIRE key s`, `PEXPIRE key ms`, `EXPIREAT key unix-s`, `PEXPIREAT key unix-ms` | 1 set, 0 absent | a time to live of 0 or less deletes the key; the `NX`/`XX`/`GT`/`LT` options are not accepted |
| `TTL key`, `PTTL key` | remaining time; -1 no expiry; -2 absent | |
| `PERSIST key` | 1 removed an expiry, 0 otherwise | |
| `KEYS pattern` | array of keys | glob pattern (`*`, `?`, `[...]`); copies every shard while holding its lock: avoid on large caches |
| `SCAN cursor [MATCH pattern] [COUNT n] [TYPE t]` | `["0", keys]` | returns all matching keys in one reply with cursor 0; same cost as `KEYS` |
| `DBSIZE` | number of entries | includes expired entries not yet reclaimed |
| `FLUSHALL`, `FLUSHDB` | `OK` | removes all entries; `ASYNC`/`SYNC` arguments are ignored |
| `PING [message]`, `ECHO message` | `PONG` or the message | |
| `AUTH [user] password` | `OK` or `WRONGPASS` | error when no ACL or credentials file is configured |
| `HELLO [2\|3] [AUTH user password] [SETNAME name]` | server information (a map in RESP3) | selects RESP2 or RESP3 for the connection |
| `RESET` | `RESET` | ends a transaction, drops watches and subscriptions, returns to RESP2 and to the default user |
| `MULTI`, `EXEC`, `DISCARD`, `WATCH key ...`, `UNWATCH` | see [Transactions](#transactions) | |
| `SUBSCRIBE channel ...`, `UNSUBSCRIBE [channel ...]`, `PSUBSCRIBE pattern ...`, `PUNSUBSCRIBE [pattern ...]` | one confirmation per channel with the number of subscriptions | see [Publish and subscribe](#publish-and-subscribe) |
| `PUBLISH channel message` | number of receivers | |
| `PUBSUB CHANNELS [pattern]`, `PUBSUB NUMSUB [channel ...]`, `PUBSUB NUMPAT` | channels with subscribers; subscribers per channel; distinct patterns | |
| `SELECT 0` | `OK` | only database 0 exists |
| `CLIENT SETNAME name`, `CLIENT GETNAME`, `CLIENT ID`, `CLIENT SETINFO ...` | | |
| `COMMAND`, `COMMAND COUNT`, `COMMAND INFO name ...` | name, arity, flags, key positions and ACL categories of every command | cluster clients use the key positions |
| `REPLICAOF host port`, `REPLICAOF NO ONE` (`SLAVEOF`) | `OK` | follow a primary's native port, or become a primary |
| `ROLE` | `master`, offset, replicas; or `slave`, host, port, state, offset | |
| `WAIT numreplicas timeout-ms` | number of replicas that acknowledged | waits for the last change made through the connection's worker; 0 waits without limit |
| `CLUSTER SLOTS\|SHARDS\|NODES\|INFO\|MYID\|KEYSLOT key\|COUNTKEYSINSLOT slot\|RELOAD` | | cluster mode; `KEYSLOT` works without it |
| `READONLY`, `READWRITE` | `OK` | cluster mode: a replica serves reads of its primary's slots to this connection |
| `CONFIG GET pattern` | `maxmemory`, `maxmemory-policy`, `save`, `appendonly` (`yes` with `--appendonly`), `appendfsync`, `databases`, `io-threads` | `CONFIG SET` is refused: use the server options |
| `ACL WHOAMI\|USERS\|LIST\|CAT [category]\|GETUSER name\|DRYRUN user command [arg ...]\|LOAD` | see [OPERATIONS.md](OPERATIONS.md#inspecting-and-reloading) | `SETUSER`, `DELUSER`, `SAVE` are refused: the ACL file is the only source of users |
| `INFO [section]` | all sections | fields in [OPERATIONS.md](OPERATIONS.md#monitoring) |
| `SAVE` | `OK` | writes the snapshot synchronously; error without `--snapshot` |
| `BGSAVE` | `Background saving started` | error while a background save runs |
| `LASTSAVE` | Unix time of the last successful snapshot, 0 if none | |
| `BGREWRITEAOF` | `Background append only file rewriting started` | error without `--appendonly` or while a rewrite runs |
| `TIME` | seconds and microseconds | |
| `QUIT` | `OK`, then the connection closes | |

Error replies follow Redis: `ERR unknown command '...'`, `ERR wrong number of arguments for '...' command`,
`ERR syntax error`, `NOAUTH Authentication required.` before authentication, `OOM command not allowed when used
memory > 'maxmemory'.` when the server runs with `--no-evict` and is full, and `ERR string exceeds maximum allowed
size` when key + value exceed the entry limit.

Not supported: lists, hashes, sets, sorted sets, streams, bitmaps, HyperLogLog, scripting, sharded pub/sub, cluster
resharding (`CLUSTER ADDSLOTS`, `MIGRATE`, `ASK`),
keyspace notifications and the ACL commands that change users (`ACL SETUSER`, `DELUSER`, `SAVE`).

## 5. Querying with the memcached protocol

Any memcached client that speaks the text protocol can be used; the binary protocol is not supported. For manual
tests, `nc` or `telnet` suffice: every command line ends with `\r\n`.

### Example session

```
$ printf 'set greeting 5 3600 5\r\nhello\r\nget greeting\r\ngets greeting\r\nadd greeting 0 0 1\r\nx\r\n' | nc -q1 127.0.0.1 16211
STORED
VALUE greeting 5 5
hello
END
VALUE greeting 5 5 1040
hello
END
NOT_STORED
$ printf 'cas greeting 0 0 3 1040\r\nnew\r\ncas greeting 0 0 3 1040\r\nold\r\n' | nc -q1 127.0.0.1 16211
STORED
EXISTS
$ printf 'set hits 0 0 1\r\n0\r\nincr hits 10\r\ndecr hits 3\r\ntouch hits 60\r\ndelete hits\r\n' | nc -q1 127.0.0.1 16211
STORED
10
7
TOUCHED
DELETED
```

In `set <key> <flags> <exptime> <bytes>`, `flags` is a 32-bit number stored with the value and returned by `get`;
`exptime` is seconds from now, or a Unix time when larger than 30 days (2,592,000 seconds), as in memcached; 0 means
no expiry and a negative value expires the item at once.

### Classic commands

| Command | Replies |
|---|---|
| `get <key>*`, `gets <key>*` | `VALUE <key> <flags> <bytes> [<cas>]`, data, ..., `END` |
| `gat <exptime> <key>*`, `gats <exptime> <key>*` | as `get`/`gets`, and sets the expiry |
| `set`, `add`, `replace`, `append`, `prepend <key> <flags> <exptime> <bytes> [noreply]` + data | `STORED`, `NOT_STORED` |
| `cas <key> <flags> <exptime> <bytes> <cas> [noreply]` + data | `STORED`, `EXISTS` (changed since `gets`), `NOT_FOUND` |
| `delete <key> [noreply]` | `DELETED`, `NOT_FOUND` |
| `incr`, `decr <key> <delta> [noreply]` | new value, `NOT_FOUND`, `CLIENT_ERROR cannot increment or decrement non-numeric value` |
| `touch <key> <exptime> [noreply]` | `TOUCHED`, `NOT_FOUND` |
| `flush_all [noreply]` | `OK` (immediate; a delay argument is refused) |
| `stats`, `stats reset` | `STAT <name> <value>` lines and `END` ([OPERATIONS.md](OPERATIONS.md#monitoring)); `RESET` |
| `version` | `VERSION 1.6.0-arenakv-<version>` |
| `verbosity <n> [noreply]` | `OK` (no effect) |
| `quit` | closes the connection |

`incr` and `decr` follow memcached: the value is an unsigned 64-bit decimal number, `incr` wraps around at 2^64, and
`decr` stops at 0. An unknown command answers `ERROR`; malformed commands answer `CLIENT_ERROR ...`; a data block
larger than the entry limit answers `SERVER_ERROR object too large for cache`, and a full cache with `--no-evict`
answers `SERVER_ERROR out of memory storing object`.

### Meta commands

The meta commands of memcached 1.6 give access to metadata with fewer round trips:

```
$ printf 'ms blk 4 T120 F7\r\ndata\r\nmg blk v f t c\r\nmg blk s k\r\nmg nothere v\r\nmn\r\n' | nc -q1 127.0.0.1 16211
HD
VA 4 f7 t120 c1031
data
HD s4 kblk
EN
MN
$ printf 'ma ctr N0 J5\r\nma ctr v D10\r\nmd blk\r\nmd blk q\r\nmn\r\n' | nc -q1 127.0.0.1 16211
HD
VA 2
15
HD
MN
```

| Command | Flags | Replies |
|---|---|---|
| `mg <key> <flags>*` | `v` value, `f` client flags, `c` CAS, `t` remaining TTL (-1: none), `s` size, `k` key, `O<token>` opaque, `q` quiet (no `EN`), `T<exptime>` update the expiry | `VA <bytes> <flags>` + data, `HD <flags>`, `EN` (miss) |
| `ms <key> <bytes> <flags>*` + data | `T<exptime>`, `F<flags>`, `C<cas>` compare-and-swap, `M<mode>` with `S` set, `E` add, `R` replace, `A` append, `P` prepend; `c` return CAS, `k`, `O`, `q` | `HD`, `NS` (not stored), `EX` (CAS mismatch), `NF` (CAS on a missing key) |
| `md <key> <flags>*` | `C<cas>` delete only if unchanged, `k`, `O`, `q` | `HD`, `NF`, `EX` |
| `ma <key> <flags>*` | `D<delta>` (default 1), `M<mode>` `I`/`+` increment or `D`/`-` decrement, `N<exptime>` create when missing with initial value `J<n>` (default 0), `T<exptime>`, `v` return the value, `t`, `c`, `k`, `O`, `q` | `HD`, `VA` + value, `NF` |
| `mn` | | `MN` (marks the end of a batch of quiet commands) |

Not supported: the binary protocol, base64 keys (flag `b`), invalidation (`I`), vivify-on-miss on `mg` (`N`),
explicit CAS on `ms` (`E`), `me`, and a delayed `flush_all`.

## 6. Querying with the native protocol

The native protocol carries one request and one response per rdmaduct message ([`PROTOCOL.md`](PROTOCOL.md)). It
is the only protocol that runs over RDMA. Two ways to use it are provided: the `akv_cli` program and the C client
library.

### akv_cli

```
$ akv_cli --port 17600 --ttl 60 --flags 3 set blk-1 hello
$ akv_cli --port 17600 meta blk-1
length 5
flags 3
cas 2085
ttl_ms 59999
$ akv_cli --port 17600 append blk-1 " world"
$ akv_cli --port 17600 get blk-1
hello world
$ akv_cli --port 17600 add blk-1 x; echo "exit status $?"
akv_cli: EXISTS
exit status 1
$ akv_cli --port 17600 set blob - < block.bin           # value from standard input (binary-safe)
$ akv_cli --port 17600 get blob > copy.bin
$ akv_cli --port 17600 incr ctr 5                     # ctr holds 15 from the meta example above
20
$ akv_cli --transport rdma --host 192.0.2.10 --port 7600 get blk-1     # over RDMA
hello world
```

| Option | Default | Meaning |
|---|---|---|
| `--host HOST`, `--port PORT` | 127.0.0.1, 7600 | server; over RDMA the address of the server's RDMA interface |
| `--transport tcp\|rdma\|auto` | tcp | must match the server's transport |
| `--auth CREDENTIALS` | | `user:password`, or a password of the user `default` |
| `--ttl SECONDS`, `--flags N` | none, 0 | expiry and client flags for `set`, `add`, `replace` |
| `--max-msg BYTES` | 1048640 | message limit of the client; the smaller of client and server limit applies |
| `--tls` | off | encrypted connection to a server with `--native-tls` (`TLS=1` build) |
| `--cacert FILE` | | CA bundle the server certificate must chain to; required with `--tls` unless `--insecure` |
| `--cert FILE`, `--key FILE` | | client certificate and key, for servers started with `--tls-ca` |
| `--sni NAME` | `--host` | name or address the server certificate must match |
| `--insecure` | | do not verify the server certificate |

| Command | Output | Exit status |
|---|---|---|
| `get KEY` | the value, raw | 0, 1 when absent |
| `meta KEY` | length, flags, CAS version, remaining TTL in ms (-1: none) | 0, 1 |
| `set`, `add`, `replace`, `append`, `prepend KEY VALUE` | | 0; 1 when the condition failed (`add` of a present key, `replace` of an absent one) |
| `del KEY`, `exists KEY` | | 0, 1 when absent |
| `touch KEY SECONDS` | | 0 (0 seconds removes the expiry), 1 |
| `incr KEY DELTA` | new value | signed 64-bit, absent counts as 0 |
| `stats` | the 19 server counters, one `name value` per line | 0 |
| `flush`, `ping` | `PONG` for ping | 0 |

Exit status 2 means invalid usage and 3 a server error or a transport failure (the message names it). The server's
`--max-msg` (default 64 KiB) bounds the native message, so values larger than about 64 KiB need a larger
`--max-msg` on the server, even though the text protocols accept entries up to `--max-entry`.

### C client library

```c
#include "akvclient.h"

struct duct_opts opts;
duct_opts_default(&opts);
opts.transport = DUCT_TRANSPORT_TCP;      /* or DUCT_TRANSPORT_RDMA */
opts.max_msg = 64 * 1024;

akvc *c;
if (akvc_connect(&c, &opts, "192.0.2.10", "7600") != 0) { /* transport error */ }
if (akvc_auth(c, "app:secret") != AKVP_OK) { /* only with --auth-file */ }

struct akvp_ext x = {.ttl_ms = 3600 * 1000, .uflags = 7};
uint64_t cas;
int rc = akvc_store(c, AKVP_ADD, "block-17", 8, data, data_len, &x, &cas);   /* AKVP_EXISTS when present */

char buf[65536];
size_t n;
rc = akvc_get(c, "block-17", 8, buf, sizeof(buf), &n);
if (rc == AKVP_OK) { /* buf[0..n) holds the value */ }
else if (rc == AKVP_NOTFOUND) { /* miss or expired */ }
else if (rc < 0) { /* transport failure: close the client */ }

akvc_close(c);
```

Build: compile with `-Iarenakv/include -Irdmaduct/include` and link `arenakv/build/<arch>/libarenakv.a`,
`rdmaduct/build/<arch>/librdmaduct.a`, `-lpthread`, and for RDMA builds `-lrdmacm -libverbs`.
[`../tools/akv_cli.c`](../tools/akv_cli.c) is a complete program that uses every synchronous call.

| Function | Purpose |
|---|---|
| `akvc_connect`, `akvc_close`, `akvc_auth` | connection, authentication |
| `akvc_put`, `akvc_store` | store (`AKVP_PUT`, `ADD`, `REPLACE`, `APPEND`, `PREPEND`, `CAS` with `ext.cas`) |
| `akvc_get`, `akvc_get_meta` | read; with metadata (flags, CAS, remaining TTL) and optional new expiry |
| `akvc_del`, `akvc_exists`, `akvc_touch`, `akvc_incr`, `akvc_flush`, `akvc_stats` | |
| `akvc_post`, `akvc_push`, `akvc_next`, `akvc_window`, `akvc_outstanding` | pipelining: up to `akvc_window()` requests in flight, answered in order |
| `akvc_fd`, `akvc_arm`, `akvc_direct_fd`, `akvc_wait_events` | integration into an event loop |
| `akvc_pool_create`, `akvc_pool_pick`, `akvc_pool_index`, `akvc_pool_close` | several servers with consistent hashing; an entry `primary,replica,...` names a primary and its replicas |
| `akvc_pool_pick_read`, `akvc_pool_reconnect`, `akvc_role` | reads from a replica; after a transport error, connect to the entry's server that now reports itself as primary (after `REPLICAOF NO ONE`); the role of a server |

Return values: `AKVP_OK` (0) or another `akvp_status` (positive) when the server answered, a negative error code
when the transport failed; after a negative result the client must be closed. A client is used by one thread at a
time; use one client per thread. The header [`../include/akvclient.h`](../include/akvclient.h) documents each call.

## 7. Limits of keys and values

| Limit | Value |
|---|---|
| Key length | 1 to 65535 bytes (native, RESP); 1 to 250 bytes without spaces or control characters (memcached) |
| Key + value | `min(--max-entry, capacity / shards / 2)`; default 1 MiB |
| Native message | key + value + header (12 bytes, 36 with expiry or flags) must fit `--max-msg` of both sides; default 64 KiB |
| Memcached command line | 2048 bytes |
| Client flags | 32 bits |
| Expiry resolution | milliseconds (native, RESP `PX`), seconds (memcached) |
| Databases | one (RESP `SELECT 0`) |

A successful store does not guarantee that a later read hits: entries can be evicted when memory is full or expire.
A read never returns an older value than the last successful store of the key.
