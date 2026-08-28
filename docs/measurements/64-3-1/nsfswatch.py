#!/usr/bin/env python3
"""64-3-1 Stage B gate sampler: NSFS's swap state, continuously, read-only.

Samples OUCBQFL / OUCBSFL / OUCBNDS *and* ASCBSTOR / OUCBSWC / ASCBFMCT.
QFL alone cannot carry the gate: QFL=00 before and after is equally
consistent with "no transition began" and with "one began and completed
between samples" -- that is how 64-0e's null turned out unreadable and why
ASCBSTOR became the instrument. A COMPLETED cycle shows retrospectively in
ASCBSTOR (and in OUCBSWC) even when QFL=80 is missed.

Offsets: SYS1.AMODGEN(IHAASCB)/(IRAOUCB) via docs/measurements/64-3-0/cblayout.py
(17/17 and 13/13).  Identity asserted on every sample.
"""
import base64, os, re, sys, time, urllib.request
CVTASVT, ASVTMAXU, ASVTENTY, ASVTAVAI = 0x22C, 0x204, 0x210, 0x80000000
ASCBASID, ASCBSTOR, ASCBOUCB, ASCBJBNI, ASCBJBNS, ASCBFMCT = 0x24,0x2C,0x90,0xAC,0xB0,0x98
OUCBQFL, OUCBSFL, OUCBAFL, OUCBASCB, OUCBSWC, OUCBNDS = 0x10,0x11,0x13,0x28,0x26,0x84
# Credentials come from the environment, never argv: an argv password is
# visible to every `ps` on the box for the life of the run.
USER   = os.environ["MBT_MVS_USER"]
PASW   = os.environ["MBT_MVS_PASS"]
TARGET = sys.argv[1]
SECS   = int(sys.argv[2]) if len(sys.argv) > 2 else 300
_auth = base64.b64encode(("%s:%s"%(USER,PASW)).encode()).decode()
def dm(a,n):
    url="http://mvsdev.lan:8080/.dm?m=%X&l=%d&c=16"%(a,n)
    rq=urllib.request.Request(url,headers={"Authorization":"Basic "+_auth})
    b=re.sub(r"<[^>]*>","",urllib.request.urlopen(rq,timeout=25).read().decode("latin-1"))
    o=bytearray()
    for L in b.split("\n"):
        m=re.match(r"\s*\+[0-9A-F]{5}\s+((?:[0-9A-F]{8}\s*){1,4})",L)
        if m:o+=bytes.fromhex(m.group(1).replace(" ",""))
    return bytes(o[:n])
def w(b,o): return int.from_bytes(b[o:o+4],"big")
def h(b,o): return int.from_bytes(b[o:o+2],"big")
cvt=w(dm(0x10,4),0); asvt=w(dm(cvt+CVTASVT,4),0); maxu=w(dm(asvt+ASVTMAXU,4),0)
ent=dm(asvt+ASVTENTY,maxu*4); ascb=None
for i in range(maxu):
    e=w(ent,i*4)
    if e & ASVTAVAI or not e: continue
    a=e & 0xFFFFFF; blk=dm(a,0xB8)
    if blk[0:4]!=b"\xC1\xE2\xC3\xC2": continue
    # JBNI is the initiated-job name, JBNS the started-task name: an STC has
    # its name in JBNS, so both are tried (the survey's swapsurvey.py rule).
    p=(w(blk,ASCBJBNI)&0xFFFFFF) or (w(blk,ASCBJBNS)&0xFFFFFF)
    if p and dm(p,8).decode("cp037","replace").strip()==TARGET: ascb=a; break
if ascb is None:
    print("%s NOT FOUND"%TARGET); sys.exit(2)
print("watching %s ascb=%06X for %ds"%(TARGET,ascb,SECS))
print("%-8s %-8s %-4s %-4s %-4s %-4s %-5s %s"%("time","ASCBSTOR","QFL","SFL","NDS","SWC","FMCT","note"))
t0=time.time(); stors=set(); swcs=set(); goo=0; n=0; peak=0; prev=None
while time.time()-t0 < SECS:
    try:
        a=dm(ascb,0xB8); o=w(a,ASCBOUCB)&0xFFFFFF
        u=dm(o,0x90) if o else None
        if u is None or u[0:4]!=b"\xD6\xE4\xC3\xC2" or (w(u,OUCBASCB)&0xFFFFFF)!=ascb:
            print("%s IDENTITY FAILED"%time.strftime("%H:%M:%S")); time.sleep(2); continue
        stor=w(a,ASCBSTOR); qfl=u[OUCBQFL]; sfl=u[OUCBSFL]
        nds=h(u,OUCBNDS); swc=h(u,OUCBSWC); fm=h(a,ASCBFMCT)
        stors.add(stor); swcs.add(swc); n+=1; peak=max(peak,fm)
        if qfl & 0x80: goo+=1
        note=[]
        if prev is not None and prev!=stor: note.append("*** ASCBSTOR CHANGED %08X -> %08X"%(prev,stor))
        if qfl & 0x80: note.append("*** QFL GOO")
        if not (sfl & 0x80): note.append("!!! NSW CLEAR")
        prev=stor
        print("%-8s %08X %02X   %02X   %-4d %-4d %-5d %s"%(time.strftime("%H:%M:%S"),
              stor,qfl,sfl,nds,swc,fm," ".join(note)))
    except Exception as ex:
        print("%s read error: %s"%(time.strftime("%H:%M:%S"),ex))
    time.sleep(3)
print()
print("SUMMARY over %d samples: distinct ASCBSTOR=%d  distinct OUCBSWC=%d"
      "  QFL=GOO seen=%d  peak ASCBFMCT=%d frames (%d KB)"
      %(n,len(stors),len(swcs),goo,peak,peak*4))
