#!/usr/bin/env python3
"""64-3-0: is any address space on this stand non-swappable, and what does
NSFS cost?  Read-only, /.dm only.  ASCB and OUCB are SQA (common), so no DAT
walk is needed -- but identity is asserted, never assumed (OUCBASCB must
equal the ASCB we chased from; the 64-0c LSQA trap).

Offsets:
  ASCB  -- SYS1.AMODGEN(IHAASCB): ASCBASID X'24' ASCBSTOR X'2C' ASCBOUCB X'90'
           ASCBJBNI X'AC' ASCBJBNS X'B0' ASCBFLG1 X'67'
           ASCBNSWP EQU X'01' "PROGRAM IS NON SWAPPABLE"
  ASVT  -- CVTASVT X'22C', ASVTMAXU X'204', ASVTENTY X'210' (job CBOFF6)
  OUCB  -- SYS1.AMODGEN(IRAOUCB), layout computed by oucblayout.py, whose
           17-offset positive control against the CBOFF-proved values passes.
           OUCBSFL X'11' (NSW BIT0 / NSWI BIT4 / PVL BIT5)
           OUCBAFL X'13' (ASW BIT7 "AUTHORIZED FOR DONTSWAP", NWT BIT6)
           OUCBPSO X'4C' OUCBWSS X'4E' OUCBNDS X'84' OUCBQFL X'10'
"""
import base64, re, sys, time, urllib.request

CVTASVT, ASVTMAXU, ASVTENTY, ASVTAVAI = 0x22C, 0x204, 0x210, 0x80000000
ASCBASID, ASCBSTOR, ASCBOUCB = 0x24, 0x2C, 0x90
ASCBJBNI, ASCBJBNS, ASCBFLG1 = 0xAC, 0xB0, 0x67
ASCBNSWP = 0x01
OUCBQFL, OUCBSFL, OUCBAFL = 0x10, 0x11, 0x13
OUCBPSO, OUCBWSS, OUCBNDS, OUCBASCB = 0x4C, 0x4E, 0x84, 0x28
ASCBFMCT = 0x98   # ALLOCATED PAGE FRAME COUNT (IHAASCB, cblayout 13/13)
OUCBNSW, OUCBNSWI, OUCBPVL = 0x80, 0x08, 0x04
OUCBASW, OUCBNWT = 0x01, 0x02

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

def w(b,o): return int.from_bytes(b[o:o+4],"big")
def h(b,o): return int.from_bytes(b[o:o+2],"big")

cvt  = w(dm(0x10,4),0)
asvt = w(dm(cvt+CVTASVT,4),0)
maxu = w(dm(asvt+ASVTMAXU,4),0)
print("%s  CVT=%06X ASVT=%06X ASVTMAXU=%d" % (time.strftime("%H:%M:%S"),cvt,asvt,maxu))
print()
print("%-4s %-8s %-6s %-9s %-9s %-10s %5s %5s %5s %s"
      % ("ASID","JOBNAME","ASCB","ASCBFLG1","OUCBSFL","AFL/NDS","FMCT","WSS","PSO","ASCBSTOR"))
print("%-4s %-8s %-6s %-9s %-9s %-10s %5s %5s %5s %s"
      % ("","","","(NSWP?)","(NSW?)","(dontswap)","frms","pgs","pgs",""))
print("-"*86)
ent = dm(asvt+ASVTENTY, maxu*4)
rows=[]
for i in range(maxu):
    e = w(ent, i*4)
    if e & ASVTAVAI or not e: continue
    ascb = e & 0x00FFFFFF
    a = dm(ascb, 0xB8)
    if a[0:4] != b"\xC1\xE2\xC3\xC2": continue
    # ASCBJBNI / ASCBJBNS are POINTERS to the 8-byte name, not the name
    ptr = w(a, ASCBJBNI) & 0x00FFFFFF or w(a, ASCBJBNS) & 0x00FFFFFF
    name = dm(ptr, 8).decode("cp037", "replace").strip() if ptr else "?"
    flg1 = a[ASCBFLG1]
    nswp = "NSWP" if flg1 & ASCBNSWP else "-"
    oucbp = w(a, ASCBOUCB) & 0x00FFFFFF
    sfl=afl=nds=wss=pso=qfl=None; ident="no OUCB"
    if oucbp:
        u = dm(oucbp, 0x90)
        if u[0:4] == b"\xD6\xE4\xC3\xC2":                       # 'OUCB'
            ident = "OK" if (w(u,OUCBASCB) & 0x00FFFFFF)==ascb else "MISMATCH"
            sfl,afl,nds = u[OUCBSFL],u[OUCBAFL],h(u,OUCBNDS)
            wss,pso,qfl = h(u,OUCBWSS),h(u,OUCBPSO),u[OUCBQFL]
        else:
            ident = "no eye"
    nsw = ("NSW"  if sfl is not None and sfl & OUCBNSW  else "-") if sfl is not None else "?"
    if sfl is not None and sfl & OUCBNSWI: nsw += ",NSWI"
    asw = ("ASW" if afl is not None and afl & OUCBASW else "-") if afl is not None else "?"
    fmct = h(a, ASCBFMCT)
    print("%-4X %-8s %06X %02X %-6s %02X %-6s %-3s/%-6s %5d %5s %5s %08X  %s"
          % (h(a,ASCBASID), name, ascb, flg1, nswp,
             sfl if sfl is not None else 0, nsw, asw,
             nds if nds is not None else "?", fmct,
             wss if wss is not None else "?", pso if pso is not None else "?",
             w(a,ASCBSTOR), ident))
    rows.append((name,nswp,nsw,asw,nds,wss,pso,fmct))
print()
print("NON-SWAPPABLE (ASCBNSWP set): %s"
      % (", ".join(r[0] for r in rows if r[1]=="NSWP") or "NONE"))
print("SRM non-swappable (OUCBNSW):  %s"
      % (", ".join(r[0] for r in rows if r[2].startswith("NSW")) or "NONE"))
print("Authorized for DONTSWAP (OUCBASW): %s"
      % (", ".join(r[0] for r in rows if r[3]=="ASW") or "NONE"))
print("Outstanding DONTSWAPs (OUCBNDS>0): %s"
      % (", ".join("%s=%d"%(r[0],r[4]) for r in rows if r[4]) or "NONE"))
tot = sum(r[7] for r in rows)
print("ASCBFMCT total across all address spaces: %d frames = %d KB"
      % (tot, tot*4))
print("machine real storage: MAINSIZE 16 (MB) = 4096 frames"
      "  [~/MVSCE/conf/local.cnf, the file the running hercules -f names]")
