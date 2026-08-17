import json
import struct
import sys

calls = 0
while True:
    pre = sys.stdin.buffer.read(4)
    if len(pre) < 4:
        break
    hlen = struct.unpack("<I", pre)[0]
    header = json.loads(sys.stdin.buffer.read(hlen))
    body = sys.stdin.buffer.read(header.get("body_len", 0))
    for n in header.get("blobs", []):
        sys.stdin.buffer.read(n)
    args = json.loads(body) if body else {}
    calls += 1
    n = args.get("n", 0)
    if n == 13:
        err = b"refused: n=13"
        rh = json.dumps({"ok": False, "error_len": len(err)}).encode()
        sys.stdout.buffer.write(struct.pack("<I", len(rh)) + rh + err)
        sys.stdout.buffer.flush()
        continue
    if n == 99:
        sys.exit(3)
    out = json.dumps({"user": args.get("user"), "n": n, "seen": calls}).encode()
    rh = json.dumps({"ok": True, "body_len": len(out), "blobs": []}).encode()
    sys.stdout.buffer.write(struct.pack("<I", len(rh)) + rh + out)
    sys.stdout.buffer.flush()
