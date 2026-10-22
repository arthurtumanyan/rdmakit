"""Writes seed inputs for the fuzz harnesses (small valid examples of every input format)."""
import os, struct, sys

out = sys.argv[1]

def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x82F63B78 if crc & 1 else crc >> 1
    return crc ^ 0xFFFFFFFF

def put(name, i, data):
    d = os.path.join(out, name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "seed%d" % i), "wb") as f:
        f.write(data)

acl = [
    b"user default on nopass ~* +@all &*\n",
    b"user app on >pw ~app:* %R~shared:* &news.* +@all -@dangerous\nuser r on -@all +@read ~*\n",
    b"user w on #5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8 %W~log:* -@all +set reset on\n",
    b"alice:secret\nalice:other\npw\n",
]
for i, a in enumerate(acl):
    put("acl", i, a)
for i, g in enumerate([b"user:*\x00user:42", b"h[a-c]l?o\x00hbllo", b"a\\*b*\x00a*bcd"]):
    put("glob", i, g)

# snapshot: header, one block with two records, end block
recs = b""
for k, v, ttl in [(b"a", b"hello", -1), (b"key2", b"x" * 40, 60000)]:
    recs += struct.pack("<HHIIq", len(k), 0, 7, len(v), ttl) + k + v
snap = b"AKVSNAP1" + struct.pack("<IIQ", 1, 0, 1700000000)
snap += struct.pack("<IIII", 0x314B4C42, 2, len(recs), crc32c(recs)) + recs
total = struct.pack("<Q", 2)
snap += struct.pack("<IIII", 0x314B4C42, 0, 8, crc32c(total)) + total
put("snapshot", 0, snap)

# append-only log increment file: put, touch, del, a transaction, flush
def rec(kind, key=b"", val=b"", flags=0, exp=0, txn=0, reason=0):
    return struct.pack("<BBHIIqQ", kind, reason, len(key), flags, len(val), exp, txn) + key + val
payload = (rec(1, b"a", b"1") + rec(3, b"a", exp=4102444800000) + rec(1, b"t1", b"v", txn=5) +
           rec(100, txn=5) + rec(2, b"a") + rec(4))
incr = b"AKVLOG01" + struct.pack("<IIQ", 1, 1, 1700000000)
incr += struct.pack("<IIII", 0x31474C41, 6, len(payload), crc32c(payload)) + payload
put("aof", 0, incr)

text = [
    b"\x00\x08set a 1 0 3\r\nabc\r\nget a b\r\nmg a v f c t\r\nincr a 1\r\n",
    b"\x01\x10*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\nPING\r\n",
    b"\x03\x05*3\r\n$4\r\nAUTH\r\n$1\r\nu\r\n$1\r\np\r\n*2\r\n$3\r\nACL\r\n$6\r\nWHOAMI\r\n",
]
for i, t in enumerate(text):
    put("text", i, t)
