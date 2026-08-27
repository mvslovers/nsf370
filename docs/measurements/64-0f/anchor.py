#!/usr/bin/env python3
"""64-0f: read the CSA anchor head through /.dm -- served/inflight/collisions
and a slot-state census.  NO MODIFY: a MODIFY POSTs the cib ECB that sits in
the executive's OWN ECBLIST, i.e. it would poke the mechanism under test.
Offsets mirrored from include/nsfvsvc.h (each NSFV_OFF_ASSERTed there).
usage: anchor.py USER PASS <anchor-hex> [--census]"""
import base64, re, sys, urllib.request
USER, PASW, ANCHOR = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
CENSUS = "--census" in sys.argv
_auth = base64.b64encode(("%s:%s" % (USER, PASW)).encode()).decode()
A_EYE, A_VER, A_FLAGS, A_INFL, A_SERVED = 0x00, 0x08, 0x0C, 0x18, 0x1C
A_REAPED, A_NSLOTS, A_EXH, A_COLL, A_SLOTS = 0x20, 0x28, 0x2C, 0x30, 0x38
SLOTLEN = 2144
STATE = {0: "FREE", 1: "PENDING", 2: "DONE", 3: "HELD", 4: "CLAIMED"}

def dm(addr, n):
    url = "http://mvsdev.lan:8080/.dm?m=%X&l=%d&c=16" % (addr, n)
    rq = urllib.request.Request(url, headers={"Authorization": "Basic " + _auth})
    txt = re.sub(r"<[^>]*>", "",
                 urllib.request.urlopen(rq, timeout=25).read().decode("latin-1"))
    out = bytearray()
    for line in txt.split("\n"):
        m = re.match(r"\s*\+[0-9A-F]{5}\s+((?:[0-9A-F]{8}\s*){1,4})", line)
        if m:
            out += bytes.fromhex(m.group(1).replace(" ", ""))
    return bytes(out[:n])

def w(b, o): return int.from_bytes(b[o:o+4], "big")
h = dm(ANCHOR, 0x38)
eye = h[0:8].decode("cp037", "replace")
if eye != "NSFVANCR":
    print("anchor eyecatcher %r -- REFUSING" % eye); sys.exit(2)
n = w(h, A_NSLOTS)
print("eye=%s ver=%d flags=%08X served=%d inflight=%d reaped=%d nslots=%d "
      "exhausted=%d collisions=%d"
      % (eye, w(h, A_VER), w(h, A_FLAGS), w(h, A_SERVED), w(h, A_INFL),
         w(h, A_REAPED), n, w(h, A_EXH), w(h, A_COLL)))
if CENSUS:
    c = {}
    for i in range(n):
        b = dm(ANCHOR + A_SLOTS + i * SLOTLEN, 8)
        s = STATE.get(w(b, 0), "?%d" % w(b, 0)) if len(b) >= 4 else "READFAIL"
        c[s] = c.get(s, 0) + 1
    print("census %s" % c)
