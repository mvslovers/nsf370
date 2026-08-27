#!/usr/bin/env python3
"""64-0f: the SWAP sampler -- observable 2, and the round's instrument.

64-0e compared NSFS (device up, never observed swapped in 38 min) against NSFV
(no device, swapped out on 12 of 16 samples) and read the sign REVERSED: the
outstanding CTCI read plausibly makes NSFS SWAP-RESISTANT, so the rare event in
issue #64 is "SRM began a swap-out at all", not "the swap got stuck".

That whole reading stands or falls on one field.  A 45-60 s cadence cannot see
a fast out-and-back cycle, and 64-0e's sampler did NOT read ASCBSTOR -- the
field that shows a COMPLETED cycle RETROSPECTIVELY, because the segment-table
origin is reassigned when the address space is swapped back in (64-0d measured
0F8BCC00 -> 0F923C00 across a transition).  So ASCBSTOR is sampled here from
the FIRST reading; it is not an improvement to the instrument, it IS the
instrument, and the reported number is the count of DISTINCT ASCBSTOR values
seen per arm:  1 = no completed cycle observed,  >= 2 = at least one.

TWO CADENCES.  The expensive read is the 64-slot scan (that is what cost 64-1
17-20 % of the machine), NOT the ASCB read, so:
  * every FAST_SECS  -- ONE /.dm of the ASCB: ASCBSTOR + EJST + RCTF/DSP1.
    This is what materially narrows the fast out-and-back gap.
  * every FULL_SECS  -- additionally the OUCB, for the swap-state flags
    (QFL/SFL/EFL/UFL, SRC, SWC) that name WHAT the transition is doing.

Offsets, all computed by IFOX00 over SYS1.AMODGEN, never from memory:
  ASCBSTOR=X'2C' ASCBEJST=X'40' ASCBSWCT=X'70' ASCBASID=X'24' ASCBASXB=X'6C'
  ASCBCPUS=X'20'                                     -- job CBOFF7 (64-0f)
  ASCBRCTF=X'66' ASCBFLG1=X'67' ASCBDSP1=X'72' ASCBOUCB=X'90' ASCBOUXB=X'94'
  OUCBQFL=X'10' OUCBSFL=X'11' OUCBEFL=X'15' OUCBUFL=X'17' OUCBSRC=X'25'
  OUCBSWC=X'26' OUCBASCB=X'28' OUCBLEN=X'90'         -- job CBOFF5 (64-0d)

IDENTITY, never internal consistency (a red line): every full sample re-proves
the address space by OUCB eyecatcher AND OUCBASCB == the ASCB we were armed on
AND ASCBASID == the ASID we were armed on.  A sample failing any of those is
printed as BADID and excluded from the distinct-ASCBSTOR count.

Read-only: CSA/SQA through /.dm.  No MODIFY -- a MODIFY POSTs the cib ECB that
sits in the executive's OWN ECBLIST, i.e. it would poke the mechanism under
test.

usage: swapwatch.py USER PASS <ascb-hex> <asid-hex> [fast-secs] [full-secs]
"""
import base64, re, sys, time, urllib.request

USER, PASW = sys.argv[1], sys.argv[2]
ASCB = int(sys.argv[3], 16)
ASID = int(sys.argv[4], 16)
FAST_SECS = int(sys.argv[5]) if len(sys.argv) > 5 else 5
FULL_SECS = int(sys.argv[6]) if len(sys.argv) > 6 else 45

HOST = "mvsdev.lan:8080"
_auth = base64.b64encode(("%s:%s" % (USER, PASW)).encode()).decode()

ASCBCPUS, ASCBASID, ASCBSTOR, ASCBEJST = 0x20, 0x24, 0x2C, 0x40
ASCBRCTF, ASCBFLG1, ASCBSWCT, ASCBDSP1 = 0x66, 0x67, 0x70, 0x72
ASCBOUCB, ASCBOUXB, ASCB_READ = 0x90, 0x94, 0x98
OUCBQFL, OUCBSFL, OUCBEFL, OUCBUFL = 0x10, 0x11, 0x15, 0x17
OUCBSRC, OUCBSWC, OUCBASCB, OUCBLEN = 0x25, 0x26, 0x28, 0x90

BITS_QFL  = [(0x80,"GOO"),(0x40,"GOI"),(0x20,"GOB"),(0x08,"OFF"),(0x04,"OUT")]
BITS_RCTF = [(0x80,"TMNO"),(0x40,"FRS"),(0x20,"FQU"),(0x08,"WAIT"),
             (0x04,"OUT"),(0x02,"TMLW")]
BITS_DSP1 = [(0x80,"NOQ"),(0x40,"FAIL")]


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


def w(b, o): return int.from_bytes(b[o:o+4], "big")
def h(b, o): return int.from_bytes(b[o:o+2], "big")
def dec(v, t):
    s = "".join(n + "," for m, n in t if v & m)
    return s[:-1] if s else "-"
def say(s): print("%s %s" % (time.strftime("%H:%M:%S"), s), flush=True)


stors, samples, bad, last_stor = {}, 0, 0, None
say("armed on ASCB %06X ASID %04X -- fast=%ds full=%ds" % (ASCB, ASID, FAST_SECS, FULL_SECS))
next_full = 0.0

while True:
    try:
        a = dm(ASCB, ASCB_READ)
        if len(a) < ASCB_READ:
            say("short ASCB read (%d) -- skipped" % len(a)); time.sleep(FAST_SECS); continue
        asid = h(a, ASCBASID)
        if asid != ASID:
            bad += 1
            say("BADID ASCBASID=%04X expected %04X -- sample EXCLUDED" % (asid, ASID))
            time.sleep(FAST_SECS); continue
        stor = w(a, ASCBSTOR)
        samples += 1
        stors[stor] = stors.get(stor, 0) + 1
        newstor = (last_stor is not None and stor != last_stor)
        if newstor:
            say("*** ASCBSTOR CHANGED %08X -> %08X -- a swap cycle COMPLETED ***"
                % (last_stor, stor))
        last_stor = stor

        due = time.time() >= next_full
        if due or newstor:
            next_full = time.time() + FULL_SECS
            oucbp = w(a, ASCBOUCB)
            line = ("STOR=%08X EJST=%08X%08X SWCT=%d CPUS=%d RCTF=%02X[%s]"
                    " FLG1=%02X DSP1=%02X[%s]"
                    % (stor, w(a, ASCBEJST), w(a, ASCBEJST+4), h(a, ASCBSWCT),
                       w(a, ASCBCPUS), a[ASCBRCTF], dec(a[ASCBRCTF], BITS_RCTF),
                       a[ASCBFLG1], a[ASCBDSP1], dec(a[ASCBDSP1], BITS_DSP1)))
            u = dm(oucbp, OUCBLEN) if oucbp else b""
            if len(u) >= OUCBLEN and u[0:4] == b"\xD6\xE4\xC3\xC2" and w(u, OUCBASCB) == ASCB:
                line += (" | OUCB@%06X QFL=%02X[%s] SFL=%02X EFL=%02X UFL=%02X"
                         " SRC=%02X SWC=%04X"
                         % (oucbp, u[OUCBQFL], dec(u[OUCBQFL], BITS_QFL),
                            u[OUCBSFL], u[OUCBEFL], u[OUCBUFL],
                            u[OUCBSRC], h(u, OUCBSWC)))
            else:
                bad += 1
                line += " | OUCB IDENTITY FAILED @%06X -- flags EXCLUDED" % oucbp
            say(line)
            say("   distinct ASCBSTOR so far: %d %s | samples=%d badid=%d"
                % (len(stors), sorted("%08X" % k for k in stors), samples, bad))
    except Exception as ex:
        say("read failed: %s" % ex)
    time.sleep(FAST_SECS)
