#!/usr/bin/env python3
"""Exercises arenakvd's RESP listener with the redis-py client.

    compat_redis_py.py PORT

Prints "ok" when every check passes, otherwise the first failing check.
"""
import sys

import redis


def main():
    r = redis.Redis(host="127.0.0.1", port=int(sys.argv[1]), socket_timeout=5)
    checks = []
    r.flushall()
    checks.append(("ping", r.ping() is True))
    checks.append(("set/get", r.set("a", b"\x00\x01binary") and r.get("a") == b"\x00\x01binary"))
    checks.append(("set nx", r.set("a", "x", nx=True) is None))
    checks.append(("set ex/ttl", r.set("t", "v", ex=50) and 0 < r.ttl("t") <= 50))
    checks.append(("incr", r.incr("n") == 1 and r.incrby("n", 41) == 42))
    checks.append(("mset/mget", r.mset({"k1": "1", "k2": "2"}) and r.mget("k1", "k2", "k3") == [b"1", b"2", None]))
    checks.append(("expire/persist", r.expire("k1", 100) and r.persist("k1") and r.ttl("k1") == -1))
    checks.append(("append/strlen", r.append("s", "ab") == 2 and r.append("s", "cd") == 4 and r.strlen("s") == 4))
    pipe = r.pipeline(transaction=False)
    for i in range(500):
        pipe.set(f"p{i}", i)
    for i in range(500):
        pipe.get(f"p{i}")
    res = pipe.execute()
    checks.append(("pipeline", res[:500] == [True] * 500 and res[500:] == [str(i).encode() for i in range(500)]))
    checks.append(("scan_iter", sorted(r.scan_iter(match="k*")) == [b"k1", b"k2"]))
    checks.append(("dbsize", r.dbsize() == 506))  # a t n k1 k2 s and p0..p499
    checks.append(("delete", r.delete("k1", "k2", "missing") == 2))
    checks.append(("info", "db0" in r.info()))
    checks.append(("config get", r.config_get("maxmemory").get("maxmemory", "0") != "0"))

    tx = r.pipeline(transaction=True)
    tx.set("tx", 1)
    tx.incr("tx")
    tx.get("tx")
    checks.append(("multi/exec", tx.execute() == [True, 2, b"2"]))
    other = redis.Redis(host="127.0.0.1", port=int(sys.argv[1]), socket_timeout=5)
    watch_error = False
    with r.pipeline() as w:
        w.watch("w")
        other.set("w", "changed")
        w.multi()
        w.set("w", "mine")
        try:
            w.execute()
        except redis.WatchError:
            watch_error = True
    checks.append(("watch conflict", watch_error and r.get("w") == b"changed"))

    ps = r.pubsub()
    ps.subscribe("chan")
    confirm = ps.get_message(timeout=2)
    delivered = other.publish("chan", "hi")
    msg = ps.get_message(timeout=2)
    checks.append(("pubsub", confirm is not None and confirm["type"] == "subscribe" and delivered == 1 and
                   msg is not None and msg["type"] == "message" and msg["data"] == b"hi"))
    ps.close()

    if int(redis.__version__.split(".")[0]) >= 5:
        r3 = redis.Redis(host="127.0.0.1", port=int(sys.argv[1]), socket_timeout=5, protocol=3)
        checks.append(("resp3", r3.set("r3", "v") and r3.get("r3") == b"v" and r3.get("absent3") is None and
                       isinstance(r3.config_get("maxmemory"), dict)))
    for name, ok in checks:
        if not ok:
            print(f"failed: {name}")
            return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
