# arenakv native protocol (revision 2)

The native protocol runs on top of [`rdmaduct`](../../rdmaduct) messages: one request message produces one response
message. All integers are little-endian. [`include/akvproto.h`](../include/akvproto.h) is the authoritative definition.
The commands of the text protocols (memcached, RESP) are described in [`USAGE.md`](USAGE.md), their compatibility
notes in [`OPERATIONS.md`](OPERATIONS.md#compatibility-notes).

## Request

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `op` | operation, see below |
| 1 | 1 | `flags` | bit 0 (`AKVP_F_EXT`): the extension follows the header; other bits must be 0 |
| 2 | 2 | `klen` | key length: 1 to 65535 for keyed operations, 0 otherwise |
| 4 | 4 | `reqid` | echoed in the response |
| 8 | 4 | `vlen` | value length (operation-specific) |
| 12 | 24 | extension | only with `AKVP_F_EXT` |
| 12 or 36 | klen | key | |
| ... | vlen | value | |

The message length must equal `12 + (24 if AKVP_F_EXT) + klen + vlen`.

Extension: `ttl_ms` (8, signed: > 0 milliseconds, 0 none, < 0 expire at once; `INT64_MIN` with GETM keeps the expiry),
`uflags` (4, client flags), reserved (4), `cas` (8, expected version for CAS). Without the extension all three are 0
(GETM: the expiry is kept).

## Response

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `status` |
| 1 | 1 | `flags` (0) |
| 2 | 2 | reserved (0) |
| 4 | 4 | `reqid` |
| 8 | 4 | `vlen`: payload length |
| 12 | vlen | payload |

## Operations

| Code | Name | Request | Response payload |
|---|---|---|---|
| 1 | GET | key | value |
| 2 | PUT | key, value, ext (ttl, flags) | new CAS (8) |
| 3 | DEL | key | |
| 4 | STATS | | 19 counters (8 each): engine counters of `akv_stats`, then current connections, total connections, commands, uptime in seconds |
| 5 | ADD | as PUT; only if absent (`EXISTS` otherwise) | new CAS |
| 6 | REPLACE | as PUT; only if present (`NOTFOUND` otherwise) | new CAS |
| 7 | APPEND | key, value; keeps flags and expiry | new CAS |
| 8 | PREPEND | as APPEND | new CAS |
| 9 | CAS | as PUT with ext.cas; `EXISTS` on a version mismatch | new CAS |
| 10 | INCR | key, value = 8-byte signed delta; unsigned memcached semantics (wraps, decrement stops at 0, absent key is `NOTFOUND`) | new value (8) |
| 11 | INCRS | as INCR with signed semantics (absent key counts as 0, overflow is `NOTNUM`) | new value (8) |
| 12 | TOUCH | key, ext.ttl_ms | |
| 13 | FLUSH | | |
| 14 | HAS | key | (`OK` or `NOTFOUND`) |
| 15 | GETM | key, ext.ttl_ms (`INT64_MIN` keeps the expiry) | flags (4), reserved (4), cas (8), ttl ms (8, -1 = none), value |
| 16 | MGET | value = keys, each as 2-byte length and bytes | per key: status (1), reserved (3), length (4), value |
| 17 | AUTH | value = `user:password` or a password | |
| 18 | NOOP | | |
| 19 | REPLSYNC | value = replication id (40), offset (8), listening port (2) | none: the connection becomes a replication link ([OPERATIONS.md](OPERATIONS.md#replication)) |
| 20 | ROLE | | role (1): 0 primary, 1 replica |

Revision 1 consisted of GET, PUT, DEL and STATS without the extension and with a 12-field STATS payload. Revision-1
requests are valid revision-2 requests; PUT now returns an 8-byte payload, which revision-1 clients ignore.

## Status codes

| Value | Name | Meaning |
|---|---|---|
| 0 | `AKVP_OK` | success |
| 1 | `AKVP_NOTFOUND` | key absent or expired |
| 2 | `AKVP_BADREQ` | malformed request (bad length, unknown operation, reserved flags set, empty key, value where none belongs) |
| 3 | `AKVP_TOOBIG` | entry larger than the engine accepts, or response larger than the client's message limit |
| 4 | `AKVP_ERROR` | internal error |
| 5 | `AKVP_EXISTS` | ADD of a present key, CAS mismatch |
| 6 | `AKVP_NOMEM` | no space and eviction disabled (`--no-evict`) |
| 7 | `AKVP_NOAUTH` | authentication required or failed |
| 8 | `AKVP_NOTNUM` | INCR of a non-numeric value, signed overflow |
| 9 | `AKVP_NOPERM` | the user's ACL rules do not allow the operation or the key |
| 10 | `AKVP_READONLY` | a write sent to a read-only replica |

A request shorter than 12 bytes is not answered: the server closes the connection. With credentials configured,
every operation other than AUTH and NOOP answers `AKVP_NOAUTH` until AUTH succeeds.

## Semantics

- Requests of one connection are executed and answered in order. A client may pipeline requests; the server stops
  reading a connection while it cannot deliver responses, so at most `min(recv_depth, send_depth) - 1` requests
  should be outstanding (`akvc_window()`; `akvc_post()` enforces it).
- A successful store does not guarantee that a later GET hits: entries can be evicted or expire at any time.
- The message size limit is negotiated by `rdmaduct` (the smaller of the two `max_msg` values) and bounds requests
  and responses; GET and MGET answer `AKVP_TOOBIG` when the response would exceed it.

## Versioning

The protocol has no version field. Revision 2 only added operation codes, status codes and an optional extension
announced by a flag bit, so revision-1 clients keep working. Incompatible changes require new operation codes or a
new handshake version in `rdmaduct`.
