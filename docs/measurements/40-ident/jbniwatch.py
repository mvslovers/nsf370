#!/usr/bin/env python3
"""Tight poll of one or more initiators' ASCBJBNI pointer and the 8 bytes it
points at, logged as RAW HEX as well as text.

Why raw hex: a first pass caught a transient that rendered as
'<non-printable>'.  A design built on this field would then see "the name
changed" -- the UNSAFE direction -- so what the bytes actually are during the
transition belongs on the record, not summarised away.

usage:  jbniwatch.py <ascb>[,<ascb>...] <seconds>
"""
import sys, time, importlib.util

spec = importlib.util.spec_from_file_location("a1", "arm1.py")
a1 = importlib.util.module_from_spec(spec)
_real = list(sys.argv)
sys.argv = ["arm1.py", "0"]
try:
    spec.loader.exec_module(a1)
except Exception:
    pass
sys.argv = _real

ascbs = [int(x, 16) for x in sys.argv[1].split(",")]
end = time.time() + int(sys.argv[2])
while time.time() < end:
    for ascb in ascbs:
        t = time.strftime("%H:%M:%S")
        try:
            a = a1.dm(ascb, 0xB4)
            p = a1.w(a, a1.ASCBJBNI) & 0x00FFFFFF
            if not p:
                print("%s %06X IDLE" % (t, ascb), flush=True)
                continue
            b = a1.dm(p, 8)
            txt = a1.ebc(b)
            txt = txt if a1.printable(txt) else "<non-printable>"
            print("%s %06X ptr=%06X hex=%s text='%s'"
                  % (t, ascb, p, b.hex().upper(), txt), flush=True)
        except Exception as e:
            print("%s %06X ERROR %s" % (t, ascb, e), flush=True)
