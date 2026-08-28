#!/usr/bin/env python3
"""Dump a window around the jobname buffer, and classify the private-chain
head -- the two remaining places a per-submission identity could live."""
import sys, os, importlib.util
spec = importlib.util.spec_from_file_location("a1", "arm1.py")
a1 = importlib.util.module_from_spec(spec)
_real = list(sys.argv)
sys.argv = ["arm1.py", "0"]
try: spec.loader.exec_module(a1)
except Exception: pass
sys.argv = _real

ascb = int(sys.argv[1] if len(sys.argv) > 1 else "FD0F18", 16)
a = a1.dm(ascb, 0xB4)
jbni = a1.w(a, a1.ASCBJBNI) & 0x00FFFFFF
asxb = a1.w(a, 0x6C) & 0x00FFFFFF          # ASCBASXB, IFOX00-proved (64-0d)

print("ASCB %06X  JBNI ptr %06X  ASCBASXB %06X" % (ascb, jbni, asxb))
print("  ASCBASXB classification: %s" % a1.classify(asxb))
base = (jbni - 0x18) & ~0xF
blob = a1.dm(base, 0x40)
print("  window %06X..%06X around the jobname buffer:" % (base, base + 0x40))
for i in range(0, 0x40, 16):
    hx = " ".join("%08X" % a1.w(blob, i + j) for j in (0, 4, 8, 12))
    tx = "".join(c if 32 <= ord(c) < 127 else "." for c in a1.ebc(blob[i:i+16]))
    print("    +%04X  %s  |%s|" % (base + i, hx, tx))
