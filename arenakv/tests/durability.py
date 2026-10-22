#!/usr/bin/env python3
"""Crash and restart behavior of arenakvd's persistence, driven over RESP.

  tests/durability.py BUILD_DIR

Checks:
  always     a client writes with a pipeline of 16 and counts acknowledged replies; the server is killed
             with SIGKILL; after a restart from the append-only log every acknowledged key is present
  everysec   the same; keys lost must have been acknowledged within the last 2 seconds before the kill
  rewrite    SIGKILL while BGREWRITEAOF runs under a write load; every acknowledged key is present
  warm       values written, SIGTERM, restart from the warm-restart file: identical contents (digest of all
             values) and the restart time

Servers listen on ports chosen by the kernel (port 0, read from the log). The logs are written below
BUILD_DIR, which is on a disk file system in the usual build setups, so fdatasync does real work (on tmpfs
it returns at once). SIGKILL leaves the page cache intact, so these checks show that a reply is sent only
after its change was handed to the file; durability against power loss additionally depends on fdatasync
and the device, which a test on one running machine cannot show. Exits 0 when every check passes.
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
    print(f"durability: {'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
    if not ok:
        failures += 1


class Server:
    def __init__(self, workdir, extra):
        self.log = os.path.join(workdir, "server.log")
        args = [os.path.join(BUILD, "arenakvd"), "--port", "off", "--text-addr", "127.0.0.1",
                "--resp-port", "0", "--threads", "2", "--log-level", "info", "--log-file", self.log] + extra
        open(self.log, "a").close()
        self.mark = os.path.getsize(self.log)
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        self.port = None
        for _ in range(200):
            with open(self.log) as f:
                f.seek(self.mark)
                m = re.search(r"listening: native \d+, memcached \d+, resp (\d+)", f.read())
            if m:
                self.port = int(m.group(1))
                break
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        if self.port is None:
            raise RuntimeError("server did not start: " + open(self.log).read()[-2000:])

    def log_since_start(self):
        with open(self.log) as f:
            f.seek(self.mark)
            return f.read()

    def kill(self, sig):
        self.proc.send_signal(sig)
        self.proc.wait(timeout=60)


class Resp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.buf = b""

    def send(self, *cmds):
        out = bytearray()
        for c in cmds:
            out += b"*%d\r\n" % len(c)
            for a in c:
                a = a if isinstance(a, bytes) else str(a).encode()
                out += b"$%d\r\n%s\r\n" % (len(a), a)
        self.s.sendall(out)

    def _line(self):
        while b"\r\n" not in self.buf:
            d = self.s.recv(1 << 16)
            if not d:
                raise ConnectionError("closed")
            self.buf += d
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def reply(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t in (b"+", b"-", b":"):
            return line
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            while len(self.buf) < n + 2:
                d = self.s.recv(1 << 16)
                if not d:
                    raise ConnectionError("closed")
                self.buf += d
            v, self.buf = self.buf[:n], self.buf[n + 2:]
            return v
        if t == b"*":
            return [self.reply() for _ in range(int(rest))]
        raise ValueError(line)

    def call(self, *args):
        self.send(args)
        return self.reply()


def write_until_killed(server, kill_after, rewrite=False):
    """Writes k<i> until the server dies; returns (acknowledged count, [(time, count)], kill time,
    (acknowledged, fsyncs) sampled halfway)."""
    c = Resp(server.port)
    acked = 0
    history = []
    start = time.monotonic()
    killed = False
    rewrite_sent = False
    sample = None
    try:
        while True:
            if sample is None and time.monotonic() - start > kill_after / 2:
                info = Resp(server.port).call(b"INFO").decode()
                sample = (acked, int(re.search(r"aof_fsyncs:(\d+)", info).group(1)))
            if not killed and time.monotonic() - start > kill_after:
                server.proc.send_signal(signal.SIGKILL)
                killed = True
            if rewrite and not rewrite_sent and time.monotonic() - start > kill_after / 2:
                c.send([b"BGREWRITEAOF"])
                c.reply()
                rewrite_sent = True
            c.send(*[[b"SET", b"k%d" % (acked + i), b"v%d" % (acked + i)] for i in range(16)])
            for _ in range(16):
                if c.reply() != b"+OK":
                    raise ConnectionError("unexpected reply")
                acked += 1
            history.append((time.monotonic(), acked))
    except (ConnectionError, OSError):
        pass
    server.proc.wait(timeout=30)
    return acked, history, time.monotonic(), sample


def present(port, n):
    c = Resp(port)
    found = 0
    for lo in range(0, n, 1000):
        keys = [b"k%d" % i for i in range(lo, min(n, lo + 1000))]
        vals = c.call(b"MGET", *keys)
        found += sum(1 for k, v in zip(keys, vals) if v == b"v" + k[1:])
    return found


def crash_case(name, workdir, fsync, rewrite=False):
    aof = os.path.join(workdir, name)
    opts = ["--appendonly", aof, "--appendfsync", fsync, "--capacity", "256M"]
    s = Server(workdir, opts)
    acked, history, killed_at, sample = write_until_killed(s, 4.0 if fsync == "everysec" else 1.5, rewrite)
    s2 = Server(workdir, opts)
    if name == "always":
        cfg = Resp(s2.port).call(b"CONFIG", b"GET", b"append*")
        check("CONFIG GET reports the log", cfg == [b"appendonly", b"yes", b"appendfsync", b"always"], str(cfg))
    got = present(s2.port, acked)
    log = s2.log_since_start()
    s2.kill(signal.SIGTERM)
    lost = acked - got
    if fsync == "always":
        per_sync = sample[0] / sample[1] if sample and sample[1] else 0
        check(f"{name}: no acknowledged write lost after SIGKILL", lost == 0,
              f"{acked} acknowledged, {got} present; {per_sync:.1f} writes per fdatasync (group commit)")
    else:
        recent = acked - max([n for t, n in history if t < killed_at - 2.0] or [0])
        check(f"{name}: losses limited to the last 2 seconds", 0 <= lost <= recent,
              f"{acked} acknowledged, {lost} lost, {recent} acknowledged in the last 2 s")
    return log


def warm_case(workdir):
    shm = "/dev/shm" if os.access("/dev/shm", os.W_OK) else workdir
    path = os.path.join(shm, "akv-durability-%d.warm" % os.getpid())
    opts = ["--warm-restart", path, "--capacity", "512M"]
    try:
        s = Server(workdir, opts)
        c = Resp(s.port)
        n = 60000
        for lo in range(0, n, 500):
            c.send(*[[b"SET", b"w%d" % i, os.urandom(3000)] for i in range(lo, lo + 500)])
            for _ in range(500):
                c.reply()

        def digest(port):
            r = Resp(port)
            h = hashlib.sha256()
            for lo in range(0, n, 1000):
                for v in r.call(b"MGET", *[b"w%d" % i for i in range(lo, lo + 1000)]):
                    h.update(b"-" if v is None else v)
            return h.hexdigest()

        before = digest(s.port)
        s.kill(signal.SIGTERM)
        t0 = time.monotonic()
        s2 = Server(workdir, opts)
        ready = time.monotonic() - t0
        after = digest(s2.port)
        log = s2.log_since_start()
        m = re.search(r"warm restart: (\d+) entries restored in ([0-9.]+) s", log)
        check("warm: identical contents after restart", before == after and m is not None,
              f"{m.group(1) if m else '?'} entries, index rebuilt in {m.group(2) if m else '?'} s, "
              f"server ready after {ready:.2f} s")
        info = Resp(s2.port).call(b"INFO")
        check("warm: INFO reports the restore", b"warm_restart:restored" in info)
        s2.kill(signal.SIGTERM)
    finally:
        if os.path.exists(path):
            os.unlink(path)


def main():
    workdir = tempfile.mkdtemp(prefix="durability-", dir=BUILD)
    try:
        crash_case("always", workdir, "always")
        crash_case("everysec", workdir, "everysec")
        log = crash_case("rewrite", workdir, "always", rewrite=True)
        check("rewrite: restart reads the log", "append-only log loaded" in log)
        warm_case(workdir)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
