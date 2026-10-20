# arenakv wire protocol

The protocol runs on top of [`rdmaduct`](../../rdmaduct) messages: one request message produces one response message.
All integers are little-endian. The definitions are in [`include/akvproto.h`](../include/akvproto.h), which is the
authoritative source.

## Request

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `op` | 1 GET, 2 PUT, 3 DEL, 4 STATS |
| 1 | 1 | `flags` | reserved, must be 0 |
| 2 | 2 | `klen` | key length: 1 to 65535 for GET, PUT, DEL; 0 for STATS |
| 4 | 4 | `reqid` | echoed in the response |
| 8 | 4 | `vlen` | value length; PUT only, 0 otherwise |
| 12 | klen | key | |
| 12 + klen | vlen | value | PUT only |

The message length must equal `12 + klen + vlen`.

## Response

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `status` | see below |
| 1 | 1 | `flags` | reserved, 0 |
| 2 | 2 | reserved | 0 |
| 4 | 4 | `reqid` | copy of the request identifier |
| 8 | 4 | `vlen` | payload length |
| 12 | vlen | payload | GET: the value; STATS: 96 bytes |

## Status codes

| Value | Name | Meaning |
|---|---|---|
| 0 | `AKVP_OK` | success |
| 1 | `AKVP_NOTFOUND` | GET or DEL of an absent key |
| 2 | `AKVP_BADREQ` | malformed request (bad length, unknown operation, reserved flags set, empty key, value on a GET) |
| 3 | `AKVP_TOOBIG` | entry larger than the engine accepts, or response larger than the client's message limit |
| 4 | `AKVP_ERROR` | internal error |

A request shorter than 12 bytes is not answered: the server closes the connection.

## STATS payload

Twelve 64-bit counters in this order: `entries`, `arena_bytes`, `log_bytes`, `gets`, `hits`, `puts`, `overwrites`,
`dels`, `evictions`, `reinsertions`, `index_evictions`, `rejected` (meaning: `akv_stats` in `include/akv.h`).

## Semantics

- Operations of one connection are processed in order. The server answers requests one at a time per connection;
  a client may keep several requests in flight within the credit limits of the transport and match responses by
  `reqid` (the provided client library sends one request at a time).
- `PUT` replaces an existing value. A successful `PUT` does not guarantee that a later `GET` hits: entries can be
  evicted at any time.
- The message size limit is negotiated by `rdmaduct` (the smaller of the two `max_msg` values) and bounds
  `12 + klen + vlen` of requests and `12 + vlen` of responses.

## Versioning

The protocol has no version field in this revision. Incompatible changes require a new operation code or a new
handshake version in `rdmaduct`.
