#!/usr/bin/env python3
"""Read the raw ASVT entry for one ASID -- the ground truth nsfreqx_classify
reads.  usage: asvtentry.py <asid-decimal> [expected-ascb-hex]"""
import sys, time, importlib.util
spec = importlib.util.spec_from_file_location("a1", "arm1.py")
a1 = importlib.util.module_from_spec(spec); _r=list(sys.argv); sys.argv=["arm1.py","0"]
try: spec.loader.exec_module(a1)
except Exception: pass
sys.argv=_r
asid = int(sys.argv[1]); exp = int(sys.argv[2],16) if len(sys.argv)>2 else None
cvt = a1.w(a1.dm(0x10,4),0)&0xFFFFFF
asvt= a1.w(a1.dm(cvt+a1.CVTASVT,4),0)&0xFFFFFF
maxu= a1.w(a1.dm(asvt+a1.ASVTMAXU,4),0)
e   = a1.w(a1.dm(asvt+a1.ASVTENTY+(asid-1)*4,4),0)
avail = bool(e & a1.ASVTAVAI)
verdict = "DEAD (avail bit)" if avail else \
          ("LIVE" if (exp is None or (e & 0xFFFFFF)==exp) else "DEAD (ascb mismatch)")
print("%s asid=%d asvtenty[%d]=%08X avail=%s ascb=%06X expected=%s -> %s"
      % (time.strftime("%H:%M:%S"), asid, asid-1, e, avail, e & 0xFFFFFF,
         ("%06X"%exp) if exp else "-", verdict))
