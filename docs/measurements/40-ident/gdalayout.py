#!/usr/bin/env python3
"""40-IDENT: the GDA offsets that bound the private area, computed with the
SAME parser 64-3-0 built (cblayout.layout) rather than a second one.

GATE: CSAPQEP must reproduce at +8 -- the offset 64-3-0 proved live and used
to size the CSA pool (CVT+X'230' -> GDA+8 CSAPQEP -> PQE).  If the control
does not reproduce, the derived offsets are unusable (CLAUDE.md 8.5).
"""
import sys, importlib.util

spec = importlib.util.spec_from_file_location("cb", "../64-3-0/cblayout.py")
cb = importlib.util.module_from_spec(spec)
sys.argv = ["cblayout.py", "."]           # the module runs its own checks on import
try:
    spec.loader.exec_module(cb)
except SystemExit:
    pass

sym = cb.layout("IHAGDA.txt", "GVSMFLAG")

KNOWN  = {"CSAPQEP": 0x08}                 # proved live by 64-3-0
DERIVE = ["VRDREG", "VRPQEP", "PASTRT", "PASIZE", "SQASPQEP", "SQASPLFT"]

print("=== IHAGDA : POSITIVE CONTROL ===")
bad = 0
for k, v in sorted(KNOWN.items(), key=lambda x: x[1]):
    got = sym.get(k); ok = got == v; bad += (not ok)
    print("  %-9s expect %#04x  computed %s  %s" %
          (k, v, ("%#04x" % got) if got is not None else "MISSING",
           "ok" if ok else "*** MISMATCH ***"))
print("  ---> %d/%d reproduce" % (len(KNOWN) - bad, len(KNOWN)))
if bad:
    print("  COMPUTATION WRONG -- derived offsets unusable."); sys.exit(1)
print("  derived: " + "  ".join("%s=%#04x" % (d, sym[d]) for d in DERIVE))
