#!/usr/bin/env python3
"""Resharding of an arenakvd cluster while clients use it: CLUSTER SETSLOT, ASK, MIGRATE and akv_reshard.

  tests/reshard.py BUILD_DIR

Three primaries and a replica of the first, each with its own copy of the map file. Checks:
  protocol    a slot marked MIGRATING on its owner and IMPORTING on another node: a key still on the owner is
              served there, a missing key answers ASK with the importing node; the importing node answers
              MOVED without ASKING and serves the key after ASKING; MIGRATE moves a key (then the owner
              answers ASK for it); keys of one slot of which only some have moved answer TRYAGAIN; SETSLOT
              NODE is refused while the old owner holds keys of the slot; SETSLOT on a non-owner and with an
              unknown node is refused; DUMP and RESTORE carry flags and time to live; CLUSTER NODES shows the
              migration states
  reshard     akv_reshard moves 2,000 slots from the first primary to the second while redis-py's
              RedisCluster writes versioned values and kvpace --driver resp-cluster runs: every acknowledged
              write is readable afterwards with its last value, kvpace reports no error, all 20,000 keys
              loaded before are intact, every node (the replica included) reports the new owner, the moved
              keys are gone from the old owner and present on its replica's new primary
  valkey-cli  valkey-cli --cluster reshard (Docker) moves 200 more slots with their keys; all keys intact
  persistence every node, restarted, reads the new map from its rewritten file
Exits 0 when every check passes.
"""
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64"
KVPACE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "kvpace", BUILD, "kvpace")
failures = 0


def check(name, ok, detail=""):
    global failures
    print(f"reshard: {'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
    if not ok:
        failures += 1


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def keyslot(key):
    s = key.find(b"{")
    if s >= 0:
        e = key.find(b"}", s + 1)
        if e > s + 1:
            key = key[s + 1:e]
    return crc16(key) & 16383


def free_ports(n):
    socks = [socket.socket() for _ in range(n)]
    for s in socks:
        s.bind(("127.0.0.1", 0))
    ports = [s.getsockname()[1] for s in socks]
    for s in socks:
        s.close()
    return ports


class Resp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.buf = b""

    def call(self, *args):
        out = bytearray(b"*%d\r\n" % len(args))
        for a in args:
            a = a if isinstance(a, bytes) else str(a).encode()
            out += b"$%d\r\n%s\r\n" % (len(a), a)
        self.s.sendall(out)
        return self.reply()

    def _fill(self):
        d = self.s.recv(1 << 16)
        if not d:
            raise ConnectionError("closed")
        self.buf += d

    def reply(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        t, rest = line[:1], line[1:]
        if t in (b"+", b"-"):
            return line
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            while len(self.buf) < n + 2:
                self._fill()
            v, self.buf = self.buf[:n], self.buf[n + 2:]
            return v
        if t == b"*":
            n = int(rest)
            return None if n < 0 else [self.reply() for _ in range(n)]
        raise ValueError(line)


class Node:
    def __init__(self, workdir, nid, resp, native):
        self.workdir, self.nid, self.resp, self.native = workdir, nid, resp, native
        self.conf = os.path.join(workdir, nid + ".conf")
        self.proc = None

    def start(self):
        log = os.path.join(self.workdir, self.nid + ".log")
        self.proc = subprocess.Popen(
            [os.path.join(BUILD, "arenakvd"), "--transport", "tcp", "--addr", "127.0.0.1", "--port", str(self.native),
             "--text-addr", "127.0.0.1", "--resp-port", str(self.resp), "--threads", "2", "--capacity", "128M",
             "--cluster-config", self.conf, "--cluster-node-id", self.nid, "--log-file", log],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        for _ in range(200):
            try:
                if Resp(self.resp).call("PING") == b"+PONG":
                    return
            except OSError:
                pass
            time.sleep(0.05)
        raise RuntimeError(self.nid + " did not start")

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            self.proc.wait(timeout=30)

    def owners(self):
        """Slot ranges of CLUSTER SLOTS as {(start, end): port}."""
        return {(s[0], s[1]): s[2][1] for s in Resp(self.resp).call("CLUSTER", "SLOTS")}


def owner_of(node, slot):
    for (a, b), port in node.owners().items():
        if a <= slot <= b:
            return port
    return None


def key_in_slot(slot, prefix="k"):
    i = 0
    while True:
        k = b"%s%d" % (prefix.encode(), i)
        if keyslot(k) == slot:
            return k
        i += 1


def protocol_checks(n):
    c1, c2, c3 = Resp(n[0].resp), Resp(n[1].resp), Resp(n[2].resp)
    slot = 100  # owned by n1
    here, gone = key_in_slot(slot, "here"), key_in_slot(slot, "gone")
    c1.call("SET", here, "1")
    check("protocol: SETSLOT MIGRATING on a non-owner is refused",
          c2.call("CLUSTER", "SETSLOT", slot, "MIGRATING", "n3").startswith(b"-ERR I'm not the owner"))
    check("protocol: SETSLOT with an unknown node is refused",
          c1.call("CLUSTER", "SETSLOT", slot, "MIGRATING", "nobody").startswith(b"-ERR I don't know about node"))
    check("protocol: SETSLOT IMPORTING on the target", c3.call("CLUSTER", "SETSLOT", slot, "IMPORTING", "n1") == b"+OK")
    check("protocol: SETSLOT MIGRATING on the owner", c1.call("CLUSTER", "SETSLOT", slot, "MIGRATING", "n3") == b"+OK")
    nodes = c1.call("CLUSTER", "NODES").decode()
    check("protocol: CLUSTER NODES shows the migration", "[%d->-n3]" % slot in nodes, nodes.splitlines()[0][-40:])
    check("protocol: a key still on the owner is served there", c1.call("GET", here) == b"1")
    r = c1.call("GET", gone)
    check("protocol: a missing key answers ASK", r == b"-ASK %d 127.0.0.1:%d" % (slot, n[2].resp), str(r))
    r = c3.call("GET", here)
    check("protocol: the importing node answers MOVED without ASKING", r.startswith(b"-MOVED %d " % slot), str(r))
    check("protocol: ASKING then SET on the importing node",
          c3.call("ASKING") == b"+OK" and c3.call("SET", gone, "2") == b"+OK")
    check("protocol: ASKING applies to one command only", c3.call("GET", gone).startswith(b"-MOVED"))
    check("protocol: MIGRATE moves a key",
          c1.call("MIGRATE", "127.0.0.1", n[2].resp, here, 0, 2000) == b"+OK" and
          c1.call("GET", here) == b"-ASK %d 127.0.0.1:%d" % (slot, n[2].resp) and
          c3.call("ASKING") == b"+OK" and c3.call("GET", here) == b"1")
    check("protocol: MIGRATE of a missing key answers NOKEY",
          c1.call("MIGRATE", "127.0.0.1", n[2].resp, here, 0, 2000) == b"+NOKEY")
    a1, a2 = b"{tag}a", b"{tag}b"
    tslot = keyslot(a1)
    owner = next(x for x in n[:3] if owner_of(x, tslot) == x.resp)
    other = next(x for x in n[:3] if x is not owner)
    co, cx = Resp(owner.resp), Resp(other.resp)
    co.call("SET", a1, "x")
    co.call("SET", a2, "y")
    cx.call("CLUSTER", "SETSLOT", tslot, "IMPORTING", owner.nid)
    co.call("CLUSTER", "SETSLOT", tslot, "MIGRATING", other.nid)
    co.call("MIGRATE", "127.0.0.1", other.resp, a1, 0, 2000)
    r = co.call("MGET", a1, a2)
    check("protocol: keys of which only some have moved answer TRYAGAIN", r.startswith(b"-TRYAGAIN"), str(r))
    r = co.call("CLUSTER", "SETSLOT", tslot, "NODE", other.nid)
    check("protocol: SETSLOT NODE is refused while the owner holds keys", r.startswith(b"-ERR Can't assign"), str(r))
    co.call("MIGRATE", "127.0.0.1", other.resp, "", 0, 2000, "KEYS", a2)
    check("protocol: SETSLOT NODE after all keys moved",
          co.call("CLUSTER", "SETSLOT", tslot, "NODE", other.nid) == b"+OK" and
          cx.call("CLUSTER", "SETSLOT", tslot, "NODE", other.nid) == b"+OK" and cx.call("MGET", a1, a2) == [b"x", b"y"])
    # move slot tslot back the same way, and end the manual migration of slot 100
    co.call("CLUSTER", "SETSLOT", tslot, "IMPORTING", other.nid)
    cx.call("CLUSTER", "SETSLOT", tslot, "MIGRATING", owner.nid)
    cx.call("MIGRATE", "127.0.0.1", owner.resp, "", 0, 2000, "KEYS", a1, a2)
    back = [Resp(x.resp).call("CLUSTER", "SETSLOT", tslot, "NODE", owner.nid) for x in n]
    check("protocol: the slot moves back to its first owner on every node", back == [b"+OK"] * 4, str(back))
    for c in (c1, c3):
        c.call("CLUSTER", "SETSLOT", slot, "STABLE")
    c3.call("ASKING")
    c3.call("MIGRATE", "127.0.0.1", n[0].resp, "", 0, 2000, "KEYS", here, gone)
    check("protocol: STABLE ends the migration", c1.call("GET", gone) == b"2" and c1.call("GET", here) == b"1")
    # DUMP and RESTORE
    c1.call("SET", here, "value", "PX", 60000)
    dump = c1.call("DUMP", here)
    c1.call("DEL", here)
    check("protocol: RESTORE of a DUMP", c1.call("RESTORE", here, 30000, dump) == b"+OK" and c1.call("GET", here) == b"value"
          and 0 < c1.call("PTTL", here) <= 30000)
    check("protocol: RESTORE refuses an existing key without REPLACE",
          c1.call("RESTORE", here, 0, dump).startswith(b"-BUSYKEY"))
    check("protocol: RESTORE checks the payload",
          c1.call("RESTORE", here, 0, dump[:-1] + b"?", "REPLACE").startswith(b"-ERR DUMP payload"))


def main():
    workdir = tempfile.mkdtemp(prefix="reshard-", dir=BUILD)
    ports = free_ports(8)
    ids = ["n1", "n2", "n3", "r1"]
    n = [Node(workdir, ids[i], ports[i], ports[4 + i]) for i in range(4)]
    text = (f"node n1 127.0.0.1 {n[0].resp} {n[0].native} primary 0-5460\n"
            f"node n2 127.0.0.1 {n[1].resp} {n[1].native} primary 5461-10922\n"
            f"node n3 127.0.0.1 {n[2].resp} {n[2].native} primary 10923-16383\n"
            f"node r1 127.0.0.1 {n[3].resp} {n[3].native} replica-of n1\n")
    for x in n:
        with open(x.conf, "w") as f:
            f.write(text)
    stop = threading.Event()
    try:
        for x in n:
            x.start()
        Resp(n[3].resp).call("REPLICAOF", "127.0.0.1", n[0].native)
        protocol_checks(n)

        try:
            from redis.cluster import ClusterNode, RedisCluster
        except ImportError:
            RedisCluster = None
            print("reshard: redis-py is not installed: keys are loaded over RESP, and no versioned writer runs")

        def owner_port(slot, moved):
            """RESP port of a slot's owner before (moved False) or after the move of slots 0-1999."""
            if moved and slot < 2000:
                return n[1].resp
            return n[0].resp if slot <= 5460 else n[1].resp if slot <= 10922 else n[2].resp

        conns = {x.resp: Resp(x.resp) for x in n[:3]}
        for i in range(20000):
            k = b"load%d" % i
            conns[owner_port(keyslot(k), False)].call("SET", k, "v%d" % i)

        acked = {}
        errors = []

        def writer(t):
            w = RedisCluster(startup_nodes=[ClusterNode("127.0.0.1", n[1].resp)], socket_timeout=10)
            version = 0
            while not stop.is_set():
                version += 1
                key = "w%d-%d" % (t, random.randrange(1000))  # own keys: versions only grow
                try:
                    w.set(key, version)
                    acked[key] = version
                    got = w.get(key)
                    if got is None or int(got) < acked[key]:
                        errors.append((key, got, acked[key]))
                except Exception as e:  # a redirection the client did not follow
                    errors.append(repr(e))

        threads = [threading.Thread(target=writer, args=(t,)) for t in range(2)] if RedisCluster else []
        for t in threads:
            t.start()
        kv = None
        if os.path.exists(KVPACE):
            kv = subprocess.Popen([KVPACE, "--driver", "resp-cluster", "--port", str(n[0].resp), "--threads", "2",
                                   "--keys", "5000", "--duration", "6", "--warmup", "0", "--read-ratio", "0.5"],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        time.sleep(1)
        t0 = time.time()
        r = subprocess.run([os.path.join(BUILD, "akv_reshard"), "--from", "127.0.0.1:%d" % n[0].resp, "--to",
                            "127.0.0.1:%d" % n[1].resp, "--slots", "0-1999", "--batch", "50"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        took = time.time() - t0
        time.sleep(1)
        stop.set()
        for t in threads:
            t.join()
        out = r.stdout.decode()
        check("reshard: akv_reshard succeeds", r.returncode == 0, "%.1f s: %s" % (took, out.strip().replace("\n", "; ")))
        if RedisCluster is not None:
            check("reshard: no client error and no stale read during the move", not errors, str(errors[:3]))
            final = RedisCluster(startup_nodes=[ClusterNode("127.0.0.1", n[2].resp)], socket_timeout=10)
            lost = [k for k, v in acked.items() if final.get(k) != str(v).encode()]
            check("reshard: every acknowledged write has its last value", not lost,
                  "%d of %d keys" % (len(lost), len(acked)))
        conns = {x.resp: Resp(x.resp) for x in n[:3]}
        bad = [i for i in range(20000)
               if conns[owner_port(keyslot(b"load%d" % i), True)].call("GET", "load%d" % i) != b"v%d" % i]
        check("reshard: the 20,000 keys loaded before are intact", not bad, "%d differ" % len(bad))
        if kv is not None:
            kout = kv.communicate(timeout=60)[0].decode()
            check("reshard: kvpace resp-cluster runs through the move without errors",
                  kv.returncode == 0 and "errors=0" in kout, kout.splitlines()[1] if len(kout.splitlines()) > 1 else kout)
        views = [{(a, b): p for (a, b), p in x.owners().items()} for x in n]
        check("reshard: every node reports the new owner",
              all(owner_of(x, 0) == n[1].resp and owner_of(x, 1999) == n[1].resp and owner_of(x, 2000) == n[0].resp
                  for x in n) and all(v == views[0] for v in views),
              "; ".join("%s %s" % (x.nid, sorted(v.items())) for x, v in zip(n, views) if v != views[0]))
        c1 = Resp(n[0].resp)
        check("reshard: the old owner holds no key of a moved slot",
              all(c1.call("CLUSTER", "COUNTKEYSINSLOT", s) == 0 for s in (0, 777, 1999)))
        moved = [i for i in range(20000) if keyslot(b"load%d" % i) < 2000]
        c2 = Resp(n[1].resp)
        check("reshard: the moved keys are on the new owner",
              all(c2.call("GET", "load%d" % i) == b"v%d" % i for i in moved[:500]), "%d moved" % len(moved))
        deadline = time.time() + 10
        while time.time() < deadline and Resp(n[3].resp).call("DBSIZE") != Resp(n[0].resp).call("DBSIZE"):
            time.sleep(0.1)
        r1, p1 = Resp(n[3].resp).call("DBSIZE"), Resp(n[0].resp).call("DBSIZE")
        check("reshard: the replica of the old owner dropped the moved keys", r1 == p1, "replica %s, primary %s" % (r1, p1))

        vk_moved = False
        if shutil.which("docker") and subprocess.run(["docker", "info"], capture_output=True).returncode == 0:
            r = subprocess.run(["docker", "run", "--rm", "--network", "host", "valkey/valkey:latest", "valkey-cli",
                                "--cluster", "reshard", "127.0.0.1:%d" % n[0].resp, "--cluster-from", "n1",
                                "--cluster-to", "n3", "--cluster-slots", "200", "--cluster-yes",
                                "--cluster-pipeline", "50"], capture_output=True, text=True, timeout=300)
            tail = (r.stdout + r.stderr).strip().splitlines()[-1:]
            vk_moved = r.returncode == 0
            check("valkey-cli: --cluster reshard moves slots 2000-2199", r.returncode == 0 and
                  all(owner_of(x, 2000) == n[2].resp and owner_of(x, 2199) == n[2].resp and
                      owner_of(x, 2200) == n[0].resp for x in n), str(tail))
            conns = {x.resp: Resp(x.resp) for x in n[:3]}
            bad = []
            for i in range(20000):
                s = keyslot(b"load%d" % i)
                port = n[2].resp if 2000 <= s < 2200 else owner_port(s, True)
                if conns[port].call("GET", "load%d" % i) != b"v%d" % i:
                    bad.append(i)
            check("valkey-cli: the 20,000 keys are intact after its reshard", not bad, "%d differ" % len(bad))
        else:
            print("reshard: skip valkey-cli (no docker)")

        for x in n:
            x.stop()
            x.start()
        check("persistence: every node reads the new map from its file after a restart",
              all(owner_of(x, 0) == n[1].resp and owner_of(x, 1999) == n[1].resp and
                  owner_of(x, 2000) == (n[2].resp if vk_moved else n[0].resp) for x in n),
              open(n[2].conf).read().splitlines()[2][:60])

    finally:
        stop.set()
        for x in n:
            try:
                x.stop()
            except Exception:
                pass
        if failures:
            for x in n:
                path = os.path.join(workdir, x.nid + ".log")
                if os.path.exists(path):
                    print("---- " + x.nid + ".log")
                    print(open(path).read()[-1500:])
        shutil.rmtree(workdir, ignore_errors=True)
    sys.exit(1 if failures else 0)


main()
