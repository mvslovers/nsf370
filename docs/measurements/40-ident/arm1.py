#!/usr/bin/env python3
"""40-IDENT arm 1 -- what an ASCB records about the job running in it.

READ-ONLY.  No production code, no MODIFY, no console command.

THE TRAP THIS TOOL EXISTS TO AVOID.  /.dm runs inside HTTPD, and private /
LSQA storage is aliased at the SAME virtual address in every address space
([[nsf370-dm-reads-httpds-address-space]]).  So a jobname pointer that lands
in the private region would be dereferenced against HTTPD's storage and could
return a clean-looking 8-byte EBCDIC string that is not the answer.  This tool
therefore CLASSIFIES every pointer against the private-area window BEFORE
dereferencing it, and prints the classification whether or not it reads.

Unlike aslist.py, which takes the FIRST NON-ZERO of (JBNS, JBNI), both fields
are read and reported SEPARATELY -- a populated JBNI would otherwise be masked
behind a populated JBNS on exactly the address spaces this round is about.

Offsets, all from SYS1.AMODGEN read live this round through 64-3-0's DSECT
gate (cblayout.py, 17/17 + 13/13; gdalayout.py, 1/1):
  ASCBASID X'24'  ASCBJBNI X'AC'  ASCBJBNS X'B0'   (IFOX00-PROVED, not derived)
  ASCBFMCT X'98'  -- DS H, a HALFWORD              (derived, gate green)
  GDA PASTRT X'10'  PASIZE X'14'  CSAPQEP X'08'     (CSAPQEP proved by 64-3-0)
  CVTASVT X'22C'  CVTGDA X'230'  ASVTMAXU X'204'  ASVTENTY X'210'
"""
import base64, os, re, sys, time, urllib.request

HOST = os.environ.get("NSF_HTTPD", "mvsdev.lan:8080")
USER = os.environ["MBT_MVS_USER"]
PASW = os.environ["MBT_MVS_PASS"]
_auth = base64.b64encode(("%s:%s" % (USER, PASW)).encode()).decode()

CVTASVT, CVTGDA = 0x22C, 0x230
ASVTMAXU, ASVTENTY, ASVTAVAI = 0x204, 0x210, 0x80000000
ASCBASID, ASCBFMCT, ASCBJBNI, ASCBJBNS = 0x24, 0x98, 0xAC, 0xB0
GDAPASTRT, GDAPASIZE, GDACSAPQEP = 0x10, 0x14, 0x08
PQESIZE, PQEREGN = 0x14, 0x18


def dm(addr, n):
    url = "http://%s/.dm?m=%X&l=%d&c=16" % (HOST, addr, n)
    rq = urllib.request.Request(url, headers={"Authorization": "Basic " + _auth})
    body = re.sub(r"<[^>]*>", "",
                  urllib.request.urlopen(rq, timeout=25).read().decode("latin-1"))
    out = bytearray()
    for line in body.split("\n"):
        m = re.match(r"\s*\+[0-9A-F]{5}\s+((?:[0-9A-F]{8}\s*){1,4})", line)
        if m:
            out += bytes.fromhex(m.group(1).replace(" ", ""))
    if len(out) < n:
        raise RuntimeError("short read at %06X: got %d of %d" % (addr, len(out), n))
    return bytes(out[:n])


def w(b, o):
    return int.from_bytes(b[o:o + 4], "big")


def h(b, o):
    return int.from_bytes(b[o:o + 2], "big")


def ebc(b):
    try:
        return b.decode("cp037")
    except Exception:
        return "<undecodable>"


def printable(s):
    return all(32 <= ord(c) < 127 for c in s)


# ---------------------------------------------------------------- boundaries
cvt = w(dm(0x10, 4), 0) & 0x00FFFFFF
gda = w(dm(cvt + CVTGDA, 4), 0) & 0x00FFFFFF
g = dm(gda, 0x20)
pastrt, pasize = w(g, GDAPASTRT), w(g, GDAPASIZE)
paend = pastrt + pasize
csapqe = w(g, GDACSAPQEP) & 0x00FFFFFF

# POSITIVE CONTROL on the GDA pointer itself: the CSA PQE reached through
# GDA+8 must reproduce 64-3-0's live CSA measurement (2064 KB total).  A GDA
# read at a wrong address would not land on a PQE describing that size.
pq = dm(csapqe, 0x20)
csasize, csaregn = w(pq, PQESIZE), w(pq, PQEREGN) & 0x00FFFFFF

print("=== BOUNDARIES (common vs private), and the control on them ===")
print("  CVT       %06X" % cvt)
print("  GDA       %06X   (CVT+X'230')" % gda)
print("  PASTRT    %06X   PASIZE %08X (%d KB)" % (pastrt, pasize, pasize // 1024))
print("  private area window: [%06X .. %06X)" % (pastrt, paend))
print("  CONTROL: GDA+8 CSAPQEP -> PQE %06X  PQESIZE %d (%d KB)  PQEREGN %06X"
      % (csapqe, csasize, csasize // 1024, csaregn))
print("           64-3-0 measured CSA = 2064 KB -> %s"
      % ("REPRODUCES" if csasize // 1024 == 2064 else
         "*** DOES NOT REPRODUCE -- boundaries unusable ***"))
print()


def classify(p):
    if p == 0:
        return "ZERO"
    return "PRIVATE" if pastrt <= p < paend else "COMMON"


def show(ascb, label=""):
    a = dm(ascb, 0xB4)
    if a[0:4] != b"\xC1\xE2\xC3\xC2":
        print("  %06X  NOT AN ASCB (no eyecatcher) -- skipped" % ascb)
        return None
    asid, fmct = h(a, ASCBASID), h(a, ASCBFMCT)   # FMCT is DS H, a HALFWORD
    row = {"ascb": ascb, "asid": asid, "fmct": fmct}
    print("  ASCB %06X  ASID %04X  FMCT %d  %s" % (ascb, asid, fmct, label))
    for nm, fld in (("JBNS", ASCBJBNS), ("JBNI", ASCBJBNI)):
        p = w(a, fld) & 0x00FFFFFF
        cls = classify(p)
        val, txt = None, ""
        if cls == "COMMON":
            try:
                val = dm(p, 8)
                txt = ebc(val)
                txt = txt if printable(txt) else "<non-printable>"
            except Exception as e:
                txt = "<read failed: %s>" % e
        elif cls == "PRIVATE":
            txt = "NOT DEREFERENCED -- private region, would read HTTPD's storage"
        print("    %s ptr=%06X  %-7s  %s" % (nm, p, cls, ("'%s'" % txt) if val else txt))
        row[nm] = (p, cls, txt if val else None)
    return row


# ------------------------------------------------------------------ targets
args = sys.argv[1:]
print("=== %s  ASCBs ===" % time.strftime("%H:%M:%S"))
if args:
    for t in args:
        show(int(t, 16), "(requested)")
else:
    asvt = w(dm(cvt + CVTASVT, 4), 0) & 0x00FFFFFF
    maxu = w(dm(asvt + ASVTMAXU, 4), 0)
    ent = dm(asvt + ASVTENTY, maxu * 4)
    for i in range(maxu):
        e = w(ent, i * 4)
        if e & ASVTAVAI or not e:
            continue
        show(e & 0x00FFFFFF, "(asvt idx %d -> asid %d)" % (i, i + 1))
