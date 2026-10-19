#!/usr/bin/env python3
"""64-0d experiment 3 -- the clean design the round should have used from the start.

ONE stall, TWO treatments in order: an early NEGATIVE rung first (so a negative
result cannot be explained away as "fired too late"), then the ping as a closing
POSITIVE control in the SAME stall.  A stall in which the ping also failed would
be visible here and is invisible in experiment 1's design.
"""
import base64, os, re, subprocess, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import herc
USER, PASW, SASCB = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
RUNG_AT, PING_AT = 60, 240
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
def qfl():
    a = dm(SASCB, 0xB4); u = dm(int.from_bytes(a[0x90:0x94], "big"), 0x90)
    return u[0x10], u[0x25], int.from_bytes(a[0x2C:0x30], "big")
def say(s): print("%s %s" % (time.strftime("%H:%M:%S"), s), flush=True)
prev, flat = ejst(), 0
say("experiment 3 armed: ext at T+%ds, ping at T+%ds" % (RUNG_AT, PING_AT))
while True:
    time.sleep(5)
    cur = ejst()
    if cur != prev: prev, flat = cur, 0; continue
    flat += 1
    if flat < 3: continue
    t0 = time.time() - 15
    q, src, stor = qfl()
    say("*** STALL: QFL=%02X SRC=%02X ASCBSTOR=%08X ***" % (q, src, stor))
    done = False
    for when, what in ((RUNG_AT, "ext"), (PING_AT, "ping")):
        while time.time() - t0 < when:
            time.sleep(3)
            if ejst() != prev:
                say("*** ENDED SPONTANEOUSLY at T+%.0fs, before '%s' ***"
                    % (time.time() - t0, what)); done = True; break
        if done: break
        say(">>> INTERVENTION at T+%.0fs: %s <<<" % (time.time() - t0, what))
        if what == "ext":
            print(herc.herc(["ext"], settle=3), flush=True)
        else:
            print(subprocess.run(["ssh", "mvsdev",
                                  "ping -c 3 -W 2 192.168.200.1 2>&1 | tail -2"],
                                 capture_output=True, text=True, timeout=60).stdout,
                  flush=True)
        for _ in range(6):
            time.sleep(5)
            if ejst() != prev:
                q, src, stor = qfl()
                say("*** ENDED %.0fs after '%s' (T+%.0fs) QFL=%02X SRC=%02X"
                    " ASCBSTOR=%08X ***"
                    % (time.time() - t0 - when, what, time.time() - t0, q, src, stor))
                done = True; break
        if done: break
        say("    '%s' -> NO CHANGE, stall continues" % what)
    if what == "ext" and not done:
        pass
    if not done:
        say("neither treatment ended it within its window; leaving it to run")
    sys.exit(0)
