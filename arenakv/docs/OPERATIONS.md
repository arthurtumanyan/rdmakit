# Operating arenakvd

`arenakvd` is an experimental cache server. Run it on a trusted network only: there is no authentication or
encryption, and any client can read, overwrite and delete any entry.

## Sizing

- `--capacity` is the memory of the entry arenas. Resident memory approaches this value once the log has wrapped;
  the hash index and per-connection buffers are additional.
- `--avg-entry` sizes the hash index (about `capacity / avg-entry` slots at 75% load). A value much smaller than the
  real mean entry size wastes index memory; a value much larger makes the index too small and evicts live entries
  ("index evictions" in `STATS`).
- `--max-msg` bounds `key + value + 12`. Clients must use a `max_msg` at least as large as the largest message they
  send. Per connection the server reserves `(recv depth + send depth) * max-msg` bytes of buffers (RDMA: registered memory).
- Entries larger than half of a shard arena are refused (`AKVP_TOOBIG`): with capacity C and S shards the limit is `C / S / 2`.

## Choosing the transport and waiting mode

| Situation | Setting |
|---|---|
| machine without RDMA hardware | `--transport tcp` |
| RDMA hardware, clients on the fabric | `--transport rdma --addr <address of the RDMA interface>` |
| more connections than spare cores | `--wait event` (default) |
| few connections, one dedicated core each | `--wait poll` may lower latency |

RDMA needs memlock limits that allow registering the buffers (`ulimit -l`).

## Monitoring

`--stats SEC` prints a line with entries, log occupancy, gets, hits, puts, evictions, reinsertions and open
connections. The same counters are available to clients with the `STATS` operation (`akvc_stats`). Useful ratios:
`hits / gets` (hit ratio of lookups), `evictions / puts` (pressure), `reinsertions` (entries kept by the second
chance), `index_evictions` (index too small for the data; raise the capacity-to-index ratio by lowering `--avg-entry`).

## Shutdown

`SIGINT` or `SIGTERM` stops accepting, closes connections and exits. The cache content is not persisted.

## Known operational limits

- One thread per connection: hundreds of connections are practical, thousands are not.
- Idle connections in polling mode consume a core each; use event mode.
- No access control, no TLS, no rate limiting, no persistence.
