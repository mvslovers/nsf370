#!/usr/bin/env python3
"""M5-2c2 stage a -- which ADR-0040 row does a REAL dying client fall into?

Polls the raw ASVT entry for one ASID and classifies it exactly as
nsfreqx_classify does, against the ASCB the guard recorded at claim time:

  avail bit set          -> DEAD (row 2, available-ASID)
  assigned, ascb != rec  -> DEAD (row 3, ASID reuse with a different ASCB)
  assigned, ascb == rec  -> LIVE

READ-ONLY: /.dm only, no MODIFY, no console command.
usage: rowwatch.py <asid-dec> <recorded-ascb-hex> <seconds> [interval-sec]
"""
import importlib.util, sys, time

spec = importlib.util.spec_from_file_location("a1", "../40-ident/arm1.py")
a1 = importlib.util.module_from_spec(spec)
_argv = list(sys.argv); sys.argv = ["arm1.py", "0"]
try: spec.loader.exec_module(a1)
except Exception: pass
sys.argv = _argv

asid = int(sys.argv[1]); rec = int(sys.argv[2], 16)
secs = float(sys.argv[3]); iv = float(sys.argv[4]) if len(sys.argv) > 4 else 1.5

cvt  = a1.w(a1.dm(0x10, 4), 0) & 0xFFFFFF
asvt = a1.w(a1.dm(cvt + a1.CVTASVT, 4), 0) & 0xFFFFFF
maxu = a1.w(a1.dm(asvt + a1.ASVTMAXU, 4), 0)
off  = asvt + a1.ASVTENTY + (asid - 1) * 4
print("# asid=%d recorded_ascb=%06X asvt=%06X maxu=%d entry@%06X"
      % (asid, rec, asvt, maxu, off), flush=True)

t0 = time.time(); prev = None
while time.time() - t0 < secs:
    e = a1.w(a1.dm(off, 4), 0)
    if e & a1.ASVTAVAI:
        row = "DEAD-row2-avail"
    elif (e & 0xFFFFFF) == (rec & 0xFFFFFF):
        row = "LIVE"
    else:
        row = "DEAD-row3-mismatch"
    if row != prev:
        print("%7.1fs %s entry=%08X ascb=%06X -> %s"
              % (time.time() - t0, time.strftime("%H:%M:%S"), e,
                 e & 0xFFFFFF, row), flush=True)
        prev = row
    time.sleep(iv)
print("%7.1fs final -> %s" % (time.time() - t0, prev), flush=True)
