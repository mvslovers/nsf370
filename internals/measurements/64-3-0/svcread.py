#!/usr/bin/env python3
"""64-3-0: read SVC table entries live, read-only, /.dm only.

Chase: CVTPTR X'10' -> CVT -> cvtabend X'C8' -> SCVT -> scvtsvct X'84' -> SVCTABLE
       (libc370 include/cvt.h:193, include/ihascvt.h:67 -- the same chase the
       production STC uses in src/nsfsx.c:nsfsx_svc_steal)
Entry (libc370 include/ihascvt.h:89-102, 8 bytes):
       +0 svcepa (4) | +4 flags: svctype:3 unused:1 svcapf:1 svcesr:1
                       svcnonpreempt:1 svcassist:1 | +5 attribute | +6 lock (2)
       svctype: 0=type1 4=type2 6=type3/4 1=type6
"""
import base64, re, sys, urllib.request
USER, PASW = sys.argv[1], sys.argv[2]
_auth = base64.b64encode(("%s:%s" % (USER,PASW)).encode()).decode()
def dm(addr,n):
    url="http://mvsdev.lan:8080/.dm?m=%X&l=%d&c=16"%(addr,n)
    rq=urllib.request.Request(url,headers={"Authorization":"Basic "+_auth})
    body=re.sub(r"<[^>]*>","",urllib.request.urlopen(rq,timeout=25).read().decode("latin-1"))
    out=bytearray()
    for line in body.split("\n"):
        m=re.match(r"\s*\+[0-9A-F]{5}\s+((?:[0-9A-F]{8}\s*){1,4})",line)
        if m: out+=bytes.fromhex(m.group(1).replace(" ",""))
    return bytes(out[:n])
def w(b,o): return int.from_bytes(b[o:o+4],"big")
TYPE={0:"type1",4:"type2",6:"type3/4",1:"type6"}
cvt  = w(dm(0x10,4),0)
scvt = w(dm(cvt+0xC8,4),0) & 0x00FFFFFF
svct = w(dm(scvt+0x84,4),0) & 0x00FFFFFF
print("CVT=%06X SCVT=%06X SVCTABLE=%06X" % (cvt,scvt,svct))
print()
print("%-6s %-9s %-8s %-5s %-5s %-4s %s" %
      ("SVC","EPA","TYPE","APF","ESR","NPMT","note"))
# 95 = SYSEVENT (SYS1.AMODGEN(SYSEVENT) line 209: "SVC 95 SYSTEM RESOURCES MANAGER SVC")
# controls: 244 = the SVC 244 self-auth NSF already uses; 239 = NSF's stolen slot;
#           255 = an unused slot;  1 = WAIT (type 1);  120 = GETMAIN (type ...)
for n,note in ((95,"SYSEVENT  <-- the subject"),(244,"self-auth (NSF uses today)"),
               (239,"NSF's stolen slot"),(255,"unused slot (control)"),
               (1,"WAIT (control)"),(120,"GETMAIN/FREEMAIN (control)"),
               (99,"SVC 99 dyn alloc (control)")):
    e = dm(svct+n*8, 8)
    f = e[4]
    print("%-6d %08X  %-8s %-5s %-5s %-4s %s" %
          (n, w(e,0), TYPE.get((f>>5)&7,"?%d"%((f>>5)&7)),
           "YES" if f & 0x08 else "no",
           "YES" if f & 0x04 else "no",
           "YES" if f & 0x02 else "no", note))
