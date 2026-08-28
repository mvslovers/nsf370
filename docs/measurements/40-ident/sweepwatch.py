#!/usr/bin/env python3
"""Loop the WHOLE ASVT and report every address space with a non-zero
ASCBJBNI.  Unlike jbniwatch.py this cannot miss an address space that did not
exist when the watch started.  usage: sweepwatch.py <seconds>"""
import sys, time, importlib.util
spec = importlib.util.spec_from_file_location("a1", "arm1.py")
a1 = importlib.util.module_from_spec(spec); _r = list(sys.argv); sys.argv = ["arm1.py", "0"]
try: spec.loader.exec_module(a1)
except Exception: pass
sys.argv = _r

cvt = a1.w(a1.dm(0x10, 4), 0) & 0xFFFFFF
asvt = a1.w(a1.dm(cvt + a1.CVTASVT, 4), 0) & 0xFFFFFF
maxu = a1.w(a1.dm(asvt + a1.ASVTMAXU, 4), 0)
end = time.time() + int(sys.argv[1])
npass = 0
while time.time() < end:
    npass += 1
    ent = a1.dm(asvt + a1.ASVTENTY, maxu * 4)
    hits = []
    for i in range(maxu):
        e = a1.w(ent, i * 4)
        if e & a1.ASVTAVAI or not e: continue
        ascb = e & 0xFFFFFF
        try:
            a = a1.dm(ascb, 0xB4)
            if a[0:4] != b"\xC1\xE2\xC3\xC2": continue
            p = a1.w(a, a1.ASCBJBNI) & 0xFFFFFF
            if not p: continue
            b = a1.dm(p, 8); txt = a1.ebc(b)
            hits.append("ASID %04X ASCB %06X ptr=%06X hex=%s text='%s'"
                        % (a1.h(a, a1.ASCBASID), ascb, p, b.hex().upper(),
                           txt if a1.printable(txt) else "<non-printable>"))
        except Exception as ex:
            hits.append("ASCB %06X ERROR %s" % (ascb, ex))
    print("%s pass=%d live=%d %s" % (time.strftime("%H:%M:%S"), npass, len(hits),
                                     " | ".join(hits)), flush=True)
