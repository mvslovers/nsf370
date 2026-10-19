#!/usr/bin/env python3
"""64-1: a stall detector that still discriminates AFTER the wake-ECB reset.

64-0c/64-0d detected a stall by ASCBEJST going bit-identical.  That worked
because every stall on record was on a SPINNING instance, so flat EJST meant
"not running" against a ~1 CPU-second-per-second background.  The 64-1 reset
removes the spin, and a TCP workload disarms the STIMER heartbeat (ADR-0034),
so a CORRECTLY IDLE executive also reads EJST flat.  Healthy and stalled become
the same reading on that instrument alone.

The discriminator that survives is a PUBLISHED REQUEST THAT STAYS PENDING:
  healthy -> the POST lands, the WAIT returns, `served` moves in milliseconds
  stalled -> the POST lands, the task is non-dispatchable, nothing moves

So the criterion here is a conjunction: EJST bit-identical AND at least one slot
PENDING AND `served` frozen, for longer than PEND_SECS.

Everything read here is CSA (the anchor) or SQA (the ASCB), so /.dm reads it
directly -- no MODIFY, no POST into the mechanism under test, no cooperation
from the STC.  Read-only.

usage: stallwatch.py USER PASS <ascb-hex> <anchor-hex> [pend-secs]
"""
import base64, re, sys, time, urllib.request

USER, PASW = sys.argv[1], sys.argv[2]
SASCB  = int(sys.argv[3], 16)
ANCHOR = int(sys.argv[4], 16)
PEND_SECS = int(sys.argv[5]) if len(sys.argv) > 5 else 20

HOST = "mvsdev.lan:8080"
_auth = base64.b64encode(("%s:%s" % (USER, PASW)).encode()).decode()

# --- anchor offsets, mirrored from include/nsfvsvc.h (NSFV_OFF_ASSERTed) ---
A_EYE, A_VER, A_FLAGS, A_SECB, A_SASCB = 0x00, 0x08, 0x0C, 0x10, 0x14
A_INFL, A_SERVED, A_REAPED, A_SEPTR    = 0x18, 0x1C, 0x20, 0x24
A_NSLOTS, A_EXH, A_COLL, A_RSVD        = 0x28, 0x2C, 0x30, 0x34
A_SLOTS  = 0x38
SLOTLEN  = 2144
SL_STATE, SL_TOKEN, SL_RECB, SL_ASCB, SL_ASID, SL_XFUNC = 0, 4, 8, 12, 16, 20
STATE = {0: "FREE", 1: "PENDING", 2: "DONE", 3: "HELD", 4: "CLAIMED"}
ASCBEJST = 0x40


def dm(addr, n):
    url = "http://%s/.dm?m=%X&l=%d&c=16" % (HOST, addr, n)
    rq = urllib.request.Request(url, headers={"Authorization": "Basic " + _auth})
    txt = re.sub(r"<[^>]*>", "",
                 urllib.request.urlopen(rq, timeout=25).read().decode("latin-1"))
    out = bytearray()
    for line in txt.split("\n"):
        m = re.match(r"\s*\+[0-9A-F]{5}\s+((?:[0-9A-F]{8}\s*){1,4})", line)
        if m:
            out += bytes.fromhex(m.group(1).replace(" ", ""))
    return bytes(out[:n])


def w(b, o):
    return int.from_bytes(b[o:o + 4], "big")


def ejst():
    return int.from_bytes(dm(SASCB, 0x50)[ASCBEJST:ASCBEJST + 8], "big")


def anchor_head():
    return dm(ANCHOR, 0x38)


def pending_slots(nslots):
    """Slot state words only: one /.dm read per slot is far too slow, so read
    the head of each slot in blocks.  Only the first 24 bytes matter."""
    out = []
    for i in range(nslots):
        base = ANCHOR + A_SLOTS + i * SLOTLEN
        b = dm(base, 24)
        if len(b) < 24:
            continue
        st = w(b, SL_STATE)
        if st != 0:
            out.append((i, STATE.get(st, "?%d" % st), w(b, SL_RECB),
                        w(b, SL_ASCB), w(b, SL_ASID), w(b, SL_XFUNC)))
    return out


def any_nonfree(nslots, limit=8):
    """Cheap pre-filter: only the first `limit` slots, which is where a small
    client set lands (the claim scan takes the LOWEST free slot).  Keeps the
    poll off HTTPD's back -- 64 reads every 2 s is a measurable load on this
    stand and contaminated a CPU sample in this very round."""
    for i in range(min(limit, nslots)):
        b = dm(ANCHOR + A_SLOTS + i * SLOTLEN, 8)
        if len(b) >= 4 and w(b, SL_STATE) == 1:
            return True
    return False


def say(s):
    print("%s %s" % (time.strftime("%H:%M:%S"), s), flush=True)


h = anchor_head()
eye = h[0:8].decode("cp037", "replace")
nslots = w(h, A_NSLOTS)
say("anchor %06X eye=%s ver=%d nslots=%d flags=%08X served=%d inflight=%d"
    % (ANCHOR, eye, w(h, A_VER), nslots, w(h, A_FLAGS), w(h, A_SERVED),
       w(h, A_INFL)))
if eye != "NSFVANCR":
    say("*** anchor eyecatcher wrong -- refusing to watch ***")
    sys.exit(2)
say("armed on ASCB %06X: EJST flat AND a slot PENDING AND served frozen"
    " for >= %ds" % (SASCB, PEND_SECS))

prev_e, prev_served, flat_since, reported = ejst(), w(h, A_SERVED), None, False
while True:
    time.sleep(2)
    try:
        e = ejst()
        h = anchor_head()
    except Exception as ex:                       # transient httpd hiccup
        say("read failed: %s" % ex)
        continue
    served, infl = w(h, A_SERVED), w(h, A_INFL)
    cheap = (e == prev_e) and (served == prev_served) and any_nonfree(nslots)
    pend = [s for s in pending_slots(nslots) if s[1] == "PENDING"] if cheap else []
    quiet = cheap and bool(pend)
    if quiet:
        if flat_since is None:
            flat_since = time.time()
        held = time.time() - flat_since
        if held >= PEND_SECS and not reported:
            reported = True
            say("*** STALL: EJST flat, served=%d frozen, %d slot(s) PENDING"
                " for %ds ***" % (served, len(pend), int(held)))
            for i, st, recb, ascb, asid, xf in pend:
                say("      SLOT%-2d %s reply_ecb=%08X ascb=%06X asid=%04X"
                    " xfunc=%d" % (i, st, recb, ascb, asid, xf))
            say("      inflight=%d exhausted=%d collisions=%d"
                % (infl, w(h, A_EXH), w(h, A_COLL)))
        elif held >= PEND_SECS and int(held) % 30 < 2:
            say("    still stalled, %ds (served=%d)" % (int(held), served))
    else:
        if reported:
            say("*** ENDED after %ds -- served=%d (was %d) EJST moved=%s ***"
                % (int(time.time() - flat_since), served, prev_served,
                   e != prev_e))
        flat_since, reported = None, False
    prev_e, prev_served = e, served
