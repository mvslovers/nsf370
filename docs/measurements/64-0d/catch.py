#!/usr/bin/env python3
"""Catch the EXIT instant of a stall and read the TCBs there.

64-0c left TCBNDTS as "correlated only": it was still set at a sample taken
after the executive had demonstrably run.  The reading that decides it is the
full task chain AT the transition -- if QFL clears while SCNDY still reads
00001000, the per-task marking outlives the address-space transition and
64-0c's observation is explained rather than anomalous.
"""
import base64, os, re, subprocess, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
USER, PASW, SASCB = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
HERE = os.path.dirname(os.path.abspath(__file__))
_auth = base64.b64encode(("%s:%s" % (USER, PASW)).encode()).decode()
def dm(a, n):
    rq = urllib.request.Request("http://mvsdev.lan:8080/.dm?m=%X&l=%d&c=16" % (a, n),
                                headers={"Authorization": "Basic " + _auth})
    b = re.sub(r"<[^>]*>", "", urllib.request.urlopen(rq, timeout=25).read().decode("latin-1"))
    o = bytearray()
    for l in b.split("\n"):
        m = re.match(r"\s*\+[0-9A-F]{5}\s+((?:[0-9A-F]{8}\s*){1,4})", l)
        if m: o += bytes.fromhex(m.group(1).replace(" ", ""))
    return bytes(o[:n])
def ejst(): return int.from_bytes(dm(SASCB, 0x50)[0x40:0x48], "big")
def say(s): print("%s %s" % (time.strftime("%H:%M:%S"), s), flush=True)
prev, flat = ejst(), 0
say("transition catcher armed on ASCB %06X" % SASCB)
while True:
    time.sleep(2)
    cur = ejst()
    if cur == prev:
        flat += 1
        if flat == 15: say("...flat 30s, waiting for the exit instant")
        continue
    if flat >= 15:
        say("*** TRANSITION after %ds flat -- reading TCBs NOW ***" % (flat * 2))
        r = subprocess.run(["python3", os.path.join(HERE, "asread.py"), USER, PASW,
                            "NSFS-AT-EXIT=%X" % SASCB],
                           capture_output=True, text=True, timeout=600)
        print(r.stdout, flush=True)
        sys.exit(0)
    prev, flat = cur, 0
