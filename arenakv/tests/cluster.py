#!/usr/bin/env python3
"""The cluster protocol of arenakvd: three primaries and one replica with a fixed slot map.

  tests/cluster.py BUILD_DIR

Checks:
  keyslot     CLUSTER KEYSLOT equals the CRC16 of the Redis Cluster specification (values from the Redis
              documentation, and a reference implementation over 10,000 keys, with hash tags)
  map         CLUSTER SLOTS, CLUSTER SHARDS, CLUSTER NODES and CLUSTER INFO describe the configuration
  redirect    keys of other nodes' slots answer MOVED with the owner's address; keys of several slots in one
              command answer CROSSSLOT; a replica redirects to its primary unless READONLY was sent, and
              then serves reads; writes on a replica are redirected
  clients     redis-py RedisCluster stores and reads 2,000 keys spread over the nodes (when redis-py is
              installed), valkey-cli -c follows redirections (when Docker is available)

Ports are chosen by binding to port 0 before the servers start. Exits 0 when every check passes.
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64"
failures = 0


def check(name, ok, detail=""):
    global failures
    print(f"cluster: {'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
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
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(10)
        self.buf = b""

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
        if t in (b"+", b"-", b":"):
            return line
        if t in (b"$", b"="):
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

    def call(self, *args):
        out = b"*%d\r\n" % len(args)
        for a in args:
            a = a if isinstance(a, bytes) else str(a).encode()
            out += b"$%d\r\n%s\r\n" % (len(a), a)
        self.s.sendall(out)
        return self.reply()


def main():
    workdir = tempfile.mkdtemp(prefix="cluster-", dir=BUILD)
    procs = []
    try:
        ports = free_ports(8)
        resp = ports[0:4]
        native = ports[4:8]
        conf = os.path.join(workdir, "cluster.conf")
        with open(conf, "w") as f:
            f.write("# three primaries and a replica of the first\n")
            f.write(f"node n1 127.0.0.1 {resp[0]} {native[0]} primary 0-5460\n")
            f.write(f"node n2 127.0.0.1 {resp[1]} {native[1]} primary 5461-10922\n")
            f.write(f"node n3 127.0.0.1 {resp[2]} {native[2]} primary 10923-16383\n")
            f.write(f"node r1 127.0.0.1 {resp[3]} {native[3]} replica-of n1\n")
        for i, nid in enumerate(["n1", "n2", "n3", "r1"]):
            log = os.path.join(workdir, nid + ".log")
            procs.append(subprocess.Popen(
                [os.path.join(BUILD, "arenakvd"), "--transport", "tcp", "--addr", "127.0.0.1", "--port", str(native[i]),
                 "--text-addr", "127.0.0.1", "--resp-port", str(resp[i]), "--threads", "2", "--capacity", "64M",
                 "--cluster-config", conf, "--cluster-node-id", nid, "--log-level", "info", "--log-file", log],
                stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT))
        for i in range(4):
            for _ in range(100):
                try:
                    Resp(resp[i]).call(b"PING")
                    break
                except OSError:
                    time.sleep(0.05)
        c = [Resp(p) for p in resp]

        check("keyslot: documented values", c[0].call(b"CLUSTER", b"KEYSLOT", b"somekey") == b":11058" and
              c[0].call(b"CLUSTER", b"KEYSLOT", b"foo{hash_tag}") == b":2515")
        bad = [k for k in (b"key%d" % i for i in range(10000))
               if c[0].call(b"CLUSTER", b"KEYSLOT", k) != b":%d" % keyslot(k)]
        check("keyslot: equals the reference CRC16 for 10,000 keys", not bad, f"{len(bad)} differ")
        check("keyslot: hash tags", c[0].call(b"CLUSTER", b"KEYSLOT", b"{user1000}.following") ==
              c[0].call(b"CLUSTER", b"KEYSLOT", b"{user1000}.followers"))

        slots = c[0].call(b"CLUSTER", b"SLOTS")
        ranges = sorted(((s[0], s[1], s[2][1]) for s in slots), key=lambda r: int(r[0][1:]))
        check("map: CLUSTER SLOTS", ranges == [(b":0", b":5460", b":%d" % resp[0]), (b":5461", b":10922", b":%d" % resp[1]),
                                              (b":10923", b":16383", b":%d" % resp[2])]
              and len([s for s in slots if s[0] == b":0"][0]) == 4, str(ranges))
        info = c[1].call(b"CLUSTER", b"INFO").decode()
        check("map: CLUSTER INFO", "cluster_state:ok" in info and "cluster_known_nodes:4" in info and
              "cluster_size:3" in info)
        nodes = c[3].call(b"CLUSTER", b"NODES").decode()
        check("map: CLUSTER NODES", "myself,slave n1" in nodes and f"127.0.0.1:{resp[0]}@{native[0]} master - 0 0 1 connected 0-5460"
              in nodes, nodes[:200])
        check("map: CLUSTER SHARDS", len(c[0].call(b"CLUSTER", b"SHARDS")) == 3)
        check("map: CLUSTER MYID", c[2].call(b"CLUSTER", b"MYID") == b"n3")

        k1 = next(k for k in (b"a%d" % i for i in range(1000)) if keyslot(k) <= 5460)
        k2 = next(k for k in (b"b%d" % i for i in range(1000)) if 5461 <= keyslot(k) <= 10922)
        check("redirect: own slot served", c[0].call(b"SET", k1, b"v") == b"+OK")
        moved = c[0].call(b"SET", k2, b"v")
        check("redirect: MOVED to the owner", moved == b"-MOVED %d 127.0.0.1:%d" % (keyslot(k2), resp[1]), str(moved))
        check("redirect: CROSSSLOT", c[0].call(b"MSET", k1, b"x", k2, b"y").startswith(b"-CROSSSLOT"))
        owner = 0 if keyslot(b"t") <= 5460 else 1 if keyslot(b"t") <= 10922 else 2
        check("redirect: hash-tagged keys share a slot", c[owner].call(b"MSET", b"{t}1", b"x", b"{t}2", b"y") == b"+OK")
        for _ in range(100):  # replication of k1 to r1
            c[3].call(b"READONLY")
            if c[3].call(b"GET", k1) == b"v":
                break
            time.sleep(0.05)
        r = Resp(resp[3])
        check("redirect: replica redirects reads without READONLY",
              r.call(b"GET", k1) == b"-MOVED %d 127.0.0.1:%d" % (keyslot(k1), resp[0]))
        check("redirect: replica serves reads after READONLY", r.call(b"READONLY") == b"+OK" and r.call(b"GET", k1) == b"v")
        check("redirect: replica redirects writes", r.call(b"SET", k1, b"w").startswith(b"-MOVED"))
        check("redirect: keys of other primaries on the replica", r.call(b"GET", k2).startswith(b"-MOVED"))

        try:
            import redis
            from redis.cluster import RedisCluster
            rc = RedisCluster(host="127.0.0.1", port=resp[0], socket_timeout=5)
            for i in range(2000):
                rc.set(f"rc{i}", i)
            good = sum(1 for i in range(2000) if rc.get(f"rc{i}") == str(i).encode())
            spread = [Resp(p).call(b"DBSIZE") for p in resp[:3]]
            check("clients: redis-py RedisCluster", good == 2000, f"{good} of 2000 read back; keys per node {spread}")
            rc.close()
            del redis
        except ImportError:
            print("cluster: skip redis-py (package not installed)")
        kvpace = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "kvpace", "build",
                              os.path.basename(os.path.normpath(BUILD)), "kvpace")
        if os.path.exists(kvpace):
            for p in resp[:3]:
                Resp(p).call(b"FLUSHALL")
            out = subprocess.run([kvpace, "--driver", "resp-cluster", "--host", "127.0.0.1", "--port", str(resp[1]),
                                  "--threads", "2", "--keys", "30000", "--read-ratio", "0.5", "--multi-get", "4",
                                  "--warmup", "0", "--duration", "2"], capture_output=True, text=True)
            m = re.search(r"ops=(\d+) .*errors=(\d+)", out.stdout + out.stderr)
            sizes = [int(Resp(p).call(b"DBSIZE")[1:]) for p in resp[:3]]
            check("clients: kvpace --driver resp-cluster", out.returncode == 0 and m is not None and
                  int(m.group(1)) > 0 and m.group(2) == "0" and min(sizes) > 0,
                  f"{m.group(0) if m else out.stderr[-300:]}; keys per node {sizes}")
        else:
            print("cluster: skip kvpace (not built)")
        if shutil.which("docker") and subprocess.run(["docker", "info"], capture_output=True).returncode == 0:
            out = subprocess.run(["docker", "run", "--rm", "--network", "host", "valkey/valkey:latest", "valkey-cli", "-c",
                                  "-p", str(resp[0]), "SET", k2, "via-cli"], capture_output=True, text=True).stdout
            got = Resp(resp[1]).call(b"GET", k2)
            check("clients: valkey-cli -c follows MOVED", out.strip() == "OK" and got == b"via-cli", out.strip())
        else:
            print("cluster: skip valkey-cli (no docker)")
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                p.kill()
        shutil.rmtree(workdir, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
