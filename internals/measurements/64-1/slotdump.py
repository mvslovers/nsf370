#!/usr/bin/env python3
"""One-shot: dump every non-FREE slot of the anchor.

Exists to VALIDATE the stall detector's read path.  stallwatch.py's whole
conjunction rests on `req_state` read at ANCHOR+0x38+i*2144; if the stride or
the offset were wrong it would read FREE forever and report "quiet" straight
through a stall -- an absence indistinguishable from success (CLAUDE.md 8.5).

The free validation is the two-client gate's phase 2, which pre-claims 61 of
the 64 slots (state CLAIMED = 4).  If this prints ~61 CLAIMED rows during a
round, stride, offsets and hex parsing are all proven at once.
"""
import base64, re, sys, time, urllib.request
USER, PASW = sys.argv[1], sys.argv[2]
ANCHOR = int(sys.argv[3], 16)
A_SLOTS, SLOTLEN, A_NSLOTS = 0x38, 2144, 0x28
STATE = {0: "FREE", 1: "PENDING", 2: "DONE", 3: "HELD", 4: "CLAIMED"}
_auth = base64.b64encode(("%s:%s" % (USER, PASW)).encode()).decode()

def dm(a, n):
    rq = urllib.request.Request("http://mvsdev.lan:8080/.dm?m=%X&l=%d&c=16" % (a, n),
                                headers={"Authorization": "Basic " + _auth})
    t = re.sub(r"<[^>]*>", "", urllib.request.urlopen(rq, timeout=25).read().decode("latin-1"))
    o = bytearray()
    for l in t.split("\n"):
        m = re.match(r"\s*\+[0-9A-F]{5}\s+((?:[0-9A-F]{8}\s*){1,4})", l)
        if m: o += bytes.fromhex(m.group(1).replace(" ", ""))
    return bytes(o[:n])

def w(b, o): return int.from_bytes(b[o:o+4], "big")

h = dm(ANCHOR, 0x38)
n = w(h, A_NSLOTS)
print("%s anchor %06X eye=%s nslots=%d served=%d inflight=%d"
      % (time.strftime("%H:%M:%S"), ANCHOR, h[0:8].decode("cp037"), n,
         w(h, 0x1C), w(h, 0x18)))
counts, rows = {}, []
for i in range(n):
    b = dm(ANCHOR + A_SLOTS + i * SLOTLEN, 24)
    st = w(b, 0)
    counts[STATE.get(st, "?%d" % st)] = counts.get(STATE.get(st, "?%d" % st), 0) + 1
    if st != 0:
        rows.append("  SLOT%-2d %-8s reply_ecb=%08X ascb=%06X asid=%04X xfunc=%d"
                    % (i, STATE.get(st, "?%d" % st), w(b, 8), w(b, 12), w(b, 16), w(b, 20)))
print("  census:", counts)
for r in rows[:12]: print(r)
if len(rows) > 12: print("  ... %d more non-FREE" % (len(rows) - 12))
