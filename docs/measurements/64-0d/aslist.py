#!/usr/bin/env python3
"""Map jobname -> ASCB by walking the ASVT, and show each address space's
dispatch/swap flags.  Read-only, /.dm only (CVT/ASVT/ASCB are all common).

Offsets computed by IFOX00 over SYS1.AMODGEN, job CBOFF6 (zero diagnostics):
CVTASVT=X'22C', ASVTMAXU=X'204', ASVTENTY=X'210', ASVTAVAI=X'80',
ASCBJBNI=X'AC', ASCBJBNS=X'B0'.  ASVTMAXU/ASVTENTY independently agree with
libc370's ihaasvt.h, which the production STC already uses.
"""
import base64, re, sys, time, urllib.request
CVTASVT, ASVTMAXU, ASVTENTY, ASVTAVAI = 0x22C, 0x204, 0x210, 0x80000000
ASCBASID, ASCBJBNI, ASCBJBNS = 0x24, 0xAC, 0xB0
ASCBRCTF, ASCBFLG1, ASCBDSP1, ASCBEJST, ASCBCPUS = 0x66, 0x67, 0x72, 0x40, 0x20
BITS_RCTF = [(0x80,"TMNO"),(0x40,"FRS"),(0x20,"FQU"),(0x08,"WAIT"),
             (0x04,"OUT"),(0x02,"TMLW")]
BITS_FLG1 = [(0x10,"TERM"),(0x08,"ABNT"),(0x04,"STND"),(0x02,"TYP1"),(0x01,"NSWP")]
BITS_DSP1 = [(0x80,"NOQ"),(0x40,"FAIL")]
USER, PASW = sys.argv[1], sys.argv[2]
_auth = base64.b64encode(("%s:%s" % (USER, PASW)).encode()).decode()

def dm(addr, n):
    url = "http://mvsdev.lan:8080/.dm?m=%X&l=%d&c=16" % (addr, n)
    rq = urllib.request.Request(url, headers={"Authorization": "Basic " + _auth})
    body = re.sub(r"<[^>]*>", "",
                  urllib.request.urlopen(rq, timeout=25).read().decode("latin-1"))
    out = bytearray()
    for line in body.split("\n"):
        m = re.match(r"\s*\+[0-9A-F]{5}\s+((?:[0-9A-F]{8}\s*){1,4})", line)
        if m: out += bytes.fromhex(m.group(1).replace(" ", ""))
    return bytes(out[:n])

def w(b, o): return int.from_bytes(b[o:o+4], "big")
def h(b, o): return int.from_bytes(b[o:o+2], "big")
def dec(v, tab): return ",".join(n for m, n in tab if v & m) or "-"

cvt  = w(dm(0x10, 4), 0)
asvt = w(dm(cvt + CVTASVT, 4), 0)
hdr  = dm(asvt + ASVTMAXU, 4)
maxu = w(hdr, 0)
print("%s  CVT=%06X ASVT=%06X ASVTMAXU=%d"
      % (time.strftime("%H:%M:%S"), cvt, asvt, maxu))
ent = dm(asvt + ASVTENTY, maxu * 4)
print("ASID ASCB   JOBNAME  RCTF FLG1 DSP1  flags")
for i in range(maxu):
    e = w(ent, i * 4)
    if e & ASVTAVAI or not e: continue
    ascb = e & 0x00FFFFFF
    a = dm(ascb, 0xB4)
    if a[0:4] != b"\xC1\xE2\xC3\xC2": continue
    nm = ""
    for fld in (ASCBJBNS, ASCBJBNI):
        p = w(a, fld) & 0x00FFFFFF
        if p:
            try: nm = dm(p, 8).decode("cp037").strip()
            except Exception: nm = ""
            if nm: break
    print("%04X %06X %-8s %02X   %02X   %02X    %s | %s | %s"
          % (h(a, ASCBASID), ascb, nm or "?", a[ASCBRCTF], a[ASCBFLG1],
             a[ASCBDSP1], dec(a[ASCBRCTF], BITS_RCTF),
             dec(a[ASCBFLG1], BITS_FLG1), dec(a[ASCBDSP1], BITS_DSP1)))
