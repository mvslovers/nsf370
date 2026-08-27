#!/usr/bin/env python3
"""64-0f: fast slot sampler -- the IN-BAND validation of the stall detector.

stallwatch.py's criterion is a CONJUNCTION: EJST bit-identical AND at least one
slot req_state == PENDING AND `served` frozen.  Its PENDING conjunct is the one
that can silently never fire -- a wrong stride or offset reads FREE forever and
reports "quiet" straight through a stall.  64-1 closed that by a separate
census; this closes it IN BAND, by watching PENDING appear and clear while
`served` advances during the very arm whose silence is being quoted.

Only slots 0..LIMIT-1 are read: the claim scan takes the LOWEST free slot, so a
small client set lands there, and reading all 64 every couple of seconds is a
measurable load on this stand (it cost 64-1 a contaminated CPU sample).

usage: pendwatch.py USER PASS <anchor-hex> <seconds> [limit]"""
import base64, re, sys, time, urllib.request
USER, PASW, ANCHOR = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
SECS = int(sys.argv[4])
LIMIT = int(sys.argv[5]) if len(sys.argv) > 5 else 4
_auth = base64.b64encode(("%s:%s" % (USER, PASW)).encode()).decode()
A_SERVED, A_NSLOTS, A_SLOTS, SLOTLEN = 0x1C, 0x28, 0x38, 2144
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

end, tally, reads, s0, sN = time.time() + SECS, {}, 0, None, None
while time.time() < end:
    try:
        h = dm(ANCHOR, 0x38)
        served = w(h, A_SERVED)
        if s0 is None: s0 = served
        sN = served
        for i in range(LIMIT):
            b = dm(ANCHOR + A_SLOTS + i * SLOTLEN, 8)
            if len(b) >= 4:
                st = STATE.get(w(b, 0), "?%d" % w(b, 0))
                tally[st] = tally.get(st, 0) + 1
                reads += 1
    except Exception as ex:
        print("read failed: %s" % ex, flush=True)
print("pendwatch: %d slot reads over %ds -> %s" % (reads, SECS, tally), flush=True)
print("pendwatch: served %s -> %s (delta %s)" % (s0, sN, (sN - s0) if s0 is not None else "?"), flush=True)
print("VALIDATION: PENDING observed = %s" % ("YES" if tally.get("PENDING") else "NO"), flush=True)
