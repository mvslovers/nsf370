#!/usr/bin/env python3
"""64-0d stall experiment driver: detect passively, probe fast, fire ONE rung.

The first stall of the round ran 7 min 9 s and ended with NO intervention, so
the ladder cannot be judged by "did it end after I did X" -- the stall is
self-limiting.  Each stall is therefore ONE experiment with ONE treatment
(or none, as a control), fired at a fixed delay after detection so the
comparison is against the untreated duration and not against my typing speed.

usage: watch2.py user pass anchor RUNG DELAY
  RUNG  = a Hercules console command, or NONE for an untreated control
  DELAY = seconds after detection at which the rung is fired
"""
import base64, os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import herc, urllib.request

USER, PASW, ANCHOR = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
RUNG, DELAY = sys.argv[4], int(sys.argv[5])
PERIOD, FLAT_FOR = 5, 3
_auth = base64.b64encode(("%s:%s" % (USER, PASW)).encode()).decode()

def dm(addr, n):
    rq = urllib.request.Request("http://mvsdev.lan:8080/.dm?m=%X&l=%d&c=16" % (addr, n),
                                headers={"Authorization": "Basic " + _auth})
    body = re.sub(r"<[^>]*>", "",
                  urllib.request.urlopen(rq, timeout=25).read().decode("latin-1"))
    out = bytearray()
    for line in body.split("\n"):
        m = re.match(r"\s*\+[0-9A-F]{5}\s+((?:[0-9A-F]{8}\s*){1,4})", line)
        if m: out += bytes.fromhex(m.group(1).replace(" ", ""))
    return bytes(out[:n])

def w(b, o): return int.from_bytes(b[o:o+4], "big")
def say(s): print("%s %s" % (time.strftime("%H:%M:%S"), s), flush=True)

SASCB = w(dm(ANCHOR, 56), 20)
BITS_QFL = [(0x80,"GOO"),(0x40,"GOI"),(0x20,"GOB"),(0x08,"OFF"),(0x04,"OUT")]
BITS_RCTF= [(0x80,"TMNO"),(0x40,"FRS"),(0x20,"FQU"),(0x08,"WAIT"),(0x04,"OUT"),(0x02,"TMLW")]
def dec(v,t): return ",".join(n for m,n in t if v & m) or "-"

def probe(tag):
    """~10 s: the four things that discriminate, and nothing else."""
    a = dm(ANCHOR, 56); asc = dm(SASCB, 0xB4)
    oucbp = w(asc, 0x90); q = src = 0; ident = "?"
    if oucbp:
        u = dm(oucbp, 0x90)
        ident = "OK" if w(u, 0x28) == SASCB else "MISMATCH"
        q, src = u[0x10], u[0x25]
    dl = herc.herc(["devlist"], settle=2)
    busy = [l.split("HHC02279I ")[1].strip() for l in dl.split("\n")
            if "0:050" in l]
    say("PROBE[%s] inflight=%d served=%d | ASCB RCTF=%02X[%s] DSP1=%02X"
        " | OUCB(%s) QFL=%02X[%s] SRC=%02X"
        % (tag, w(a,24), w(a,28), asc[0x66], dec(asc[0x66],BITS_RCTF), asc[0x72],
           ident, q, dec(q,BITS_QFL), src))
    for b in busy: say("PROBE[%s]   %s" % (tag, b))

def ejst(): return int.from_bytes(dm(SASCB, 0x50)[0x40:0x48], "big")

say("EXPERIMENT: rung=%s at T+%ds ; anchor=%06X server_ascb=%06X"
    % (RUNG, DELAY, ANCHOR, SASCB))
prev, flat = ejst(), 0
while True:
    time.sleep(PERIOD)
    cur = ejst(); d = cur - prev; prev = cur
    if d > 0:
        flat = 0; continue
    flat += 1
    if flat < FLAT_FOR: continue
    t0 = time.time() - flat * PERIOD
    say("*** STALL detected (flat %ds) ***" % (flat * PERIOD))
    probe("during")
    if RUNG != "NONE":
        while time.time() - t0 < DELAY: time.sleep(2)
        say(">>> INTERVENTION: '%s' <<<" % RUNG)
        print(herc.herc([RUNG], settle=3), flush=True)
    else:
        say(">>> CONTROL: no intervention <<<")
    while True:
        time.sleep(PERIOD)
        cur = ejst()
        if cur != prev:
            say("*** ENDED after %.0fs (rung=%s fired at T+%ds) ***"
                % (time.time() - t0, RUNG, DELAY if RUNG != "NONE" else -1))
            probe("after"); prev = cur; flat = 0; break
        if time.time() - t0 > 1800:
            say("*** still stalled at 1800s -- giving up on this one ***"); break
    sys.exit(0)
