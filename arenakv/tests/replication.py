#!/usr/bin/env python3
"""Replication between arenakvd processes over the native transport (TCP), driven over RESP.

  tests/replication.py BUILD_DIR

Checks:
  sync        a replica of a primary with 20,000 keys (values, expiry, deletes, MSET, a transaction) holds
              the same contents (digest of all values) once its offset equals the primary's
  read-only   writes to the replica are refused; WAIT on the primary counts the replica
  partial     REPLICAOF to the same primary restarts the link, which continues from the replica's offset
  overflow    a replica stopped (SIGSTOP) while more than the backlog size is written resynchronizes fully
  expiry      an entry that expires on the primary is not returned by the replica
  failover    the primary is killed, one replica is promoted (REPLICAOF NO ONE), the other follows it and
              continues from its offset; writes to the new primary reach it

Servers listen on ports chosen by the kernel (read from their logs). Exits 0 when every check passes.
"""
import hashlib
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


def check(name, ok, detail=""):
    global failures
    print(f"replication: {'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
    if not ok:
        failures += 1


class Server:
    def __init__(self, workdir, name, extra):
        self.log = os.path.join(workdir, name + ".log")
        args = [os.path.join(BUILD, "arenakvd"), "--transport", "tcp", "--addr", "127.0.0.1", "--port", "0",
                "--text-addr", "127.0.0.1", "--resp-port", "0", "--threads", "2", "--capacity", "128M",
                "--log-level", "info", "--log-file", self.log] + extra
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        self.port = self.native = None
        for _ in range(200):
            if os.path.exists(self.log):
                m = re.search(r"listening: native (\d+), memcached \d+, resp (\d+)", open(self.log).read())
                if m:
                    self.native, self.port = int(m.group(1)), int(m.group(2))
                    break
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        if self.port is None:
            raise RuntimeError(name + " did not start: " + (open(self.log).read()[-2000:] if os.path.exists(self.log) else ""))

    def text(self):
        return open(self.log).read()

    def stop(self, sig=signal.SIGTERM):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
            self.proc.wait(timeout=60)


class Resp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(30)
        self.buf = b""

    def send(self, *cmds):
        out = bytearray()
        for c in cmds:
            out += b"*%d\r\n" % len(c)
            for a in c:
                a = a if isinstance(a, bytes) else str(a).encode()
                out += b"$%d\r\n%s\r\n" % (len(a), a)
        self.s.sendall(out)

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

    def call(self, *args):
        self.send(args)
        return self.reply()


def info_field(port, field):
    text = Resp(port).call(b"INFO").decode()
    m = re.search(field + r":(\S+)", text)
    return m.group(1) if m else None


def wait_synced(primary, replica, timeout=20):
    """Waits until the replica's offset equals the primary's. Returns the offset or None."""
    end = time.time() + timeout
    while time.time() < end:
        p = info_field(primary.port, "master_repl_offset")
        r = info_field(replica.port, "slave_repl_offset")
        link = info_field(replica.port, "master_link_status")
        if p is not None and p == r and link == "up":
            return int(p)
        time.sleep(0.1)
    return None


def digest(port, keys):
    c = Resp(port)
    h = hashlib.sha256()
    for lo in range(0, len(keys), 1000):
        for v in c.call(b"MGET", *keys[lo:lo + 1000]):
            h.update(b"-" if v is None else b"+" + v)
    return h.hexdigest()


def load(port, n, prefix=b"k"):
    c = Resp(port)
    for lo in range(0, n, 500):
        c.send(*[[b"SET", prefix + b"%d" % i, b"v%d" % (i * 7)] for i in range(lo, min(n, lo + 500))])
        for _ in range(min(n, lo + 500) - lo):
            c.reply()
    c.call(b"SET", b"ttl-long", b"x", b"EX", b"3600")
    c.call(b"DEL", prefix + b"5", prefix + b"6")
    c.call(b"MSET", b"m1", b"a", b"m2", b"b")
    c.send([b"MULTI"], [b"INCR", b"tx"], [b"INCR", b"tx"], [b"EXEC"])
    for _ in range(4):
        c.reply()


def main():
    workdir = tempfile.mkdtemp(prefix="replication-", dir=BUILD)
    servers = []
    try:
        p = Server(workdir, "primary", [])
        servers.append(p)
        keys = [b"k%d" % i for i in range(20000)] + [b"ttl-long", b"m1", b"m2", b"tx"]
        load(p.port, 20000)
        r1 = Server(workdir, "replica1", ["--replicaof", f"127.0.0.1:{p.native}"])
        servers.append(r1)
        off = wait_synced(p, r1)
        check("sync: replica reaches the primary's offset", off is not None, f"offset {off}")
        check("sync: identical contents", digest(p.port, keys) == digest(r1.port, keys))
        check("sync: full copy logged", "full copy, stream from offset" in p.text())

        reply = Resp(r1.port).call(b"SET", b"x", b"1")
        check("read-only: replica refuses writes", reply is not None and reply.startswith(b"-READONLY"), str(reply))
        c = Resp(p.port)
        c.call(b"SET", b"w", b"1")
        check("read-only: WAIT counts the replica", c.call(b"WAIT", b"1", b"5000") == b":1")
        check("read-only: ROLE of the primary lists the replica",
              (lambda role: role[0] == b"master" and len(role[2]) == 1)(Resp(p.port).call(b"ROLE")))
        role = Resp(r1.port).call(b"ROLE")
        check("read-only: ROLE of the replica", role[0] == b"slave" and role[3] == b"connected", str(role))

        before = p.text().count("continuing at offset")
        Resp(r1.port).call(b"REPLICAOF", b"127.0.0.1", str(p.native).encode())
        load(p.port, 2000, b"p")
        off = wait_synced(p, r1)
        check("partial: link restarted continues from the replica's offset",
              p.text().count("continuing at offset") == before + 1 and off is not None)
        keys += [b"p%d" % i for i in range(2000)]
        check("partial: identical contents", digest(p.port, keys) == digest(r1.port, keys))

        c.call(b"SET", b"short", b"v", b"PX", b"300")
        wait_synced(p, r1)
        time.sleep(0.6)
        check("expiry: expired entry not returned by the replica", Resp(r1.port).call(b"GET", b"short") is None)

        # overflow: a small backlog and a replica that pauses
        p2 = Server(workdir, "primary2", ["--repl-backlog-size", "64K"])
        servers.append(p2)
        r2 = Server(workdir, "replica2", ["--replicaof", f"127.0.0.1:{p2.native}"])
        servers.append(r2)
        load(p2.port, 1000, b"a")
        wait_synced(p2, r2)
        fulls = p2.text().count("full copy")
        r2.proc.send_signal(signal.SIGSTOP)
        big = Resp(p2.port)
        for i in range(200):
            big.call(b"SET", b"big%d" % i, os.urandom(65536))  # more than the socket buffers absorb
        time.sleep(0.5)
        r2.proc.send_signal(signal.SIGCONT)
        off = wait_synced(p2, r2, 40)
        check("overflow: a replica behind the backlog resynchronizes fully", off is not None and
              p2.text().count("full copy") == fulls + 1, f"full copies {p2.text().count('full copy') - fulls}")
        okeys = [b"a%d" % i for i in range(1000)] + [b"big%d" % i for i in range(200)]
        check("overflow: identical contents", digest(p2.port, okeys) == digest(r2.port, okeys))

        # failover: r1 and r3 follow p; p dies; r1 is promoted; r3 follows r1 and continues
        r3 = Server(workdir, "replica3", ["--replicaof", f"127.0.0.1:{p.native}"])
        servers.append(r3)
        wait_synced(p, r3)
        wait_synced(p, r1)
        p.stop(signal.SIGKILL)
        Resp(r1.port).call(b"REPLICAOF", b"NO", b"ONE")
        cont = r1.text().count("continuing at offset")
        Resp(r3.port).call(b"REPLICAOF", b"127.0.0.1", str(r1.native).encode())
        Resp(r1.port).call(b"SET", b"after-failover", b"yes")
        off = wait_synced(r1, r3)
        check("failover: the other replica continues from its offset after the promotion",
              off is not None and r1.text().count("continuing at offset") == cont + 1)
        check("failover: writes to the new primary reach it", Resp(r3.port).call(b"GET", b"after-failover") == b"yes")
        check("failover: identical contents", digest(r1.port, keys) == digest(r3.port, keys))
    finally:
        for s in servers:
            try:
                s.stop()
            except Exception:  # noqa: BLE001
                s.proc.kill()
        shutil.rmtree(workdir, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
