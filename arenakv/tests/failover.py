#!/usr/bin/env python3
"""Automatic failover of a replicated arenakvd group by three akv_sentinel monitors.

  tests/failover.py BUILD_DIR

A primary and two replicas (TCP, fixed ports on 127.0.0.1) are watched by three monitors with a down-after
time of 1 s and a failover timeout of 4 s. Checks:
  discovery   the monitors and redis-py's Sentinel client (when installed) name the primary
  failover    after the primary is killed (SIGKILL), the replica with the highest offset is promoted within
              a few seconds, the other replica follows it, every monitor names it, the keys written before
              are there and new writes reach the remaining replica
  rejoin      the old primary, restarted, is turned into a replica of the new one and catches up
  minority    with two of three monitors stopped, a killed primary is not replaced (one monitor cannot be
              elected); when the two return, the failover completes
  manual      SENTINEL FAILOVER moves the primary of a healthy group to a replica without losing a key
              acknowledged by WAIT
Exits 0 when every check passes.
"""
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64"
failures = 0
# Sanitizer builds run several times slower (ThreadSanitizer 5 to 15 times): detection times and waits are
# scaled, so that a slow but correct failover is not reported as a failure.
SLOW = 4 if "-tsan" in BUILD else 2 if "-san" in BUILD else 1
DOWN_AFTER_MS = 1000 * SLOW
FAILOVER_TIMEOUT_MS = 4000 * SLOW


def check(name, ok, detail=""):
    global failures
    print(f"failover: {'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
    if not ok:
        failures += 1


def free_ports(n):
    socks = [socket.socket() for _ in range(n)]
    for s in socks:
        s.bind(("127.0.0.1", 0))
    ports = [s.getsockname()[1] for s in socks]
    for s in socks:
        s.close()
    return ports


class Resp:
    def __init__(self, port, timeout=5):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
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

    def close(self):
        self.s.close()


def call(port, *args):
    try:
        r = Resp(port, timeout=2 * SLOW)
        try:
            return r.call(*args)
        finally:
            r.close()
    except OSError:
        return None


class Node:
    def __init__(self, workdir, name, resp, native):
        self.workdir, self.name, self.resp, self.native = workdir, name, resp, native
        self.proc = None

    def start(self, replicaof=None):
        log = os.path.join(self.workdir, self.name + ".log")
        args = [os.path.join(BUILD, "arenakvd"), "--transport", "tcp", "--addr", "127.0.0.1", "--port",
                str(self.native), "--text-addr", "127.0.0.1", "--resp-port", str(self.resp), "--threads", "2",
                "--capacity", "64M", "--log-file", log]
        if replicaof is not None:
            args += ["--replicaof", "127.0.0.1:%d" % replicaof.native]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        deadline = time.time() + 10
        while time.time() < deadline and call(self.resp, "PING") != b"+PONG":
            time.sleep(0.05)

    def stop(self, sig=signal.SIGTERM):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(sig)
            self.proc.wait(timeout=30)

    def role(self):
        r = call(self.resp, "ROLE")
        return r if isinstance(r, list) else None


class Monitor:
    def __init__(self, workdir, idx, port, nodes, peers):
        self.workdir, self.idx, self.port, self.nodes, self.peers = workdir, idx, port, nodes, peers
        self.proc = None

    def start(self):
        args = [os.path.join(BUILD, "akv_sentinel"), "--name", "grp", "--listen", "127.0.0.1:%d" % self.port,
                "--state", os.path.join(self.workdir, "sentinel%d.state" % self.idx),
                "--down-after", str(DOWN_AFTER_MS), "--failover-timeout", str(FAILOVER_TIMEOUT_MS)]
        for n in self.nodes:
            args += ["--node", "127.0.0.1:%d:%d" % (n.resp, n.native)]
        for p in self.peers:
            args += ["--peer", "127.0.0.1:%d" % p]
        self.log = open(os.path.join(self.workdir, "sentinel%d.log" % self.idx), "a")
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 10
        while time.time() < deadline and call(self.port, "PING") != b"+PONG":
            time.sleep(0.05)

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            self.proc.wait(timeout=30)
        self.log.close()

    def primary(self):
        r = call(self.port, "SENTINEL", "get-master-addr-by-name", "grp")
        return int(r[1]) if isinstance(r, list) and len(r) == 2 else None


def wait_for(cond, seconds):
    deadline = time.time() + seconds * SLOW
    while time.time() < deadline:
        v = cond()
        if v:
            return v
        time.sleep(0.1)
    return cond()


def follows(node, primary):
    r = node.role()
    return r is not None and r[0] == b"slave" and int(r[2]) == primary.native and r[3] == b"connected"


def synced(primary, replica):
    p, r = primary.role(), replica.role()
    return p is not None and r is not None and p[0] == b"master" and r[0] == b"slave" and int(r[4]) == int(p[1])


def main():
    workdir = tempfile.mkdtemp(prefix="akv-failover-")
    ports = free_ports(9)
    nodes = [Node(workdir, "n%d" % i, ports[2 * i], ports[2 * i + 1]) for i in range(3)]
    mports = ports[6:9]
    monitors = [Monitor(workdir, i, mports[i], nodes, [p for p in mports if p != mports[i]]) for i in range(3)]
    try:
        nodes[0].start()
        nodes[1].start(replicaof=nodes[0])
        nodes[2].start(replicaof=nodes[0])
        for m in monitors:
            m.start()
        c = Resp(nodes[0].resp)
        for i in range(1000):
            c.call("SET", "k%d" % i, "v%d" % i)
        c.close()
        check("replicas in sync before the failover",
              wait_for(lambda: synced(nodes[0], nodes[1]) and synced(nodes[0], nodes[2]), 10))

        # discovery
        check("monitors name the primary", all(m.primary() == nodes[0].resp for m in monitors),
              str([m.primary() for m in monitors]))
        try:
            from redis.sentinel import Sentinel
        except ImportError:
            Sentinel = None
        if Sentinel is not None:
            sen = Sentinel([("127.0.0.1", p) for p in mports], socket_timeout=2)
            check("redis-py Sentinel finds the primary", sen.discover_master("grp") == ("127.0.0.1", nodes[0].resp),
                  str(sen.discover_master("grp")))
            check("redis-py Sentinel lists the replicas",
                  sorted(p for _, p in sen.discover_slaves("grp")) == sorted([nodes[1].resp, nodes[2].resp]))

        # failover: node 2 is held back (SIGSTOP) while node 1 receives more, so node 1 has the higher offset
        nodes[2].proc.send_signal(signal.SIGSTOP)
        c = Resp(nodes[0].resp)
        for i in range(1000, 1200):
            c.call("SET", "k%d" % i, "v%d" % i)
        check("WAIT for the replica that is not stopped", c.call("WAIT", 1, 5000) >= 1)
        c.close()
        nodes[2].proc.send_signal(signal.SIGCONT)
        t0 = time.time()
        nodes[0].stop(signal.SIGKILL)
        new = wait_for(lambda: all(m.primary() == nodes[1].resp for m in monitors), 15)
        took = time.time() - t0
        check("every monitor names the promoted replica", bool(new),
              "%.1f s, monitors %s" % (took, [m.primary() for m in monitors]))
        check("failover within down-after + 5 s", took < DOWN_AFTER_MS / 1000 + 5 * SLOW, "%.1f s" % took)
        check("the replica with the highest offset is the new primary",
              wait_for(lambda: (nodes[1].role() or [b""])[0] == b"master", 5))
        check("the other replica follows the new primary", wait_for(lambda: follows(nodes[2], nodes[1]), 10))
        n1 = Resp(nodes[1].resp)
        got = [n1.call("GET", "k%d" % i) for i in (0, 999, 1000, 1199)]
        check("keys written before the failover are on the new primary",
              got == [b"v0", b"v999", b"v1000", b"v1199"], str(got))
        n1.call("SET", "after", "1")
        check("writes to the new primary reach the other replica",
              wait_for(lambda: call(nodes[2].resp, "GET", "after") == b"1", 10))
        n1.close()
        if Sentinel is not None:
            sen = Sentinel([("127.0.0.1", p) for p in mports], socket_timeout=2)
            check("redis-py Sentinel finds the new primary",
                  sen.discover_master("grp") == ("127.0.0.1", nodes[1].resp))
            primary = sen.master_for("grp", socket_timeout=2)
            primary.set("through-sentinel", "yes")
            check("a redis-py client through Sentinel writes to it", primary.get("through-sentinel") == b"yes")

        # rejoin
        nodes[0].start()
        check("the old primary rejoins as a replica of the new one",
              wait_for(lambda: follows(nodes[0], nodes[1]), 10))
        check("and catches up", wait_for(lambda: call(nodes[0].resp, "GET", "after") == b"1", 10))

        # minority: one monitor alone cannot be elected
        monitors[1].stop()
        monitors[2].stop()
        nodes[1].stop(signal.SIGKILL)
        time.sleep(DOWN_AFTER_MS / 1000 + 3 * SLOW)
        check("no failover with one of three monitors",
              monitors[0].primary() == nodes[1].resp and (nodes[0].role() or [b""])[0] == b"slave" and
              (nodes[2].role() or [b""])[0] == b"slave")
        monitors[1].start()
        monitors[2].start()
        new = wait_for(lambda: monitors[0].primary() in (nodes[0].resp, nodes[2].resp) and
                       len({m.primary() for m in monitors}) == 1, 20)
        check("failover once the monitors return", bool(new), str([m.primary() for m in monitors]))
        cur = next(n for n in nodes if n.resp == monitors[0].primary())
        other = next(n for n in (nodes[0], nodes[2]) if n is not cur)
        check("the remaining replica follows", wait_for(lambda: follows(other, cur), 10))
        nodes[1].start()
        check("the killed node rejoins", wait_for(lambda: follows(nodes[1], cur), 10))

        # manual failover of a healthy group
        c = Resp(cur.resp)
        c.call("SET", "before-manual", "1")
        check("WAIT before the manual failover", c.call("WAIT", 2, 5000) == 2)
        c.close()
        r = call(mports[0], "SENTINEL", "FAILOVER", "grp")
        check("SENTINEL FAILOVER accepted", r == b"+OK", str(r))
        moved = wait_for(lambda: len({m.primary() for m in monitors}) == 1 and monitors[0].primary() != cur.resp, 15)
        check("the primary moves to a replica", bool(moved), str([m.primary() for m in monitors]))
        now = next(n for n in nodes if n.resp == monitors[0].primary())
        check("every other node follows the new primary",
              wait_for(lambda: all(follows(n, now) for n in nodes if n is not now), 15))
        check("no acknowledged key lost", call(now.resp, "GET", "before-manual") == b"1")
    finally:
        for m in monitors:
            if m.proc is not None:
                try:
                    m.stop()
                except Exception:
                    pass
        for n in nodes:
            try:
                n.proc.send_signal(signal.SIGCONT) if n.proc is not None and n.proc.poll() is None else None
                n.stop()
            except Exception:
                pass
        races = [i for i in range(3) if os.path.exists(os.path.join(workdir, "sentinel%d.log" % i)) and
                 "ThreadSanitizer" in open(os.path.join(workdir, "sentinel%d.log" % i)).read()]
        check("no sanitizer report in the monitors' logs", not races, "monitors %s" % races)
        if failures:
            for i in range(3):
                path = os.path.join(workdir, "sentinel%d.log" % i)
                if os.path.exists(path):
                    print("---- sentinel%d.log" % i)
                    print(open(path).read()[-3000:])
        shutil.rmtree(workdir, ignore_errors=True)
    sys.exit(1 if failures else 0)


main()
