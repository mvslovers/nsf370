#!/usr/bin/env python3
"""64-3-0: compute DSECT offsets from an AMODGEN macro source, with a
positive control.  The derived offsets may be used ONLY if every
independently-proved control offset reproduces -- see CLAUDE.md 8.5.

Controls are the offsets prior IFOX00 jobs (CBOFF/CBOFF5/CBOFF6, 64-0d) proved
and which asread.py/aslist.py have used live across several rounds.
"""
import io, re, sys

def operand_size(op):
    op = op.split()[0] if op.split() else ""      # drop the trailing " -" marker
    m = re.match(r"^(\d*)([CADFHXBPZ])(L(\d+))?\s*(['(])", op) or \
        re.match(r"^(\d*)([CADFHXBPZ])(L(\d+))?$", op)
    if not m: return None
    dup = int(m.group(1)) if m.group(1) else 1
    t, ln = m.group(2), (int(m.group(4)) if m.group(4) else None)
    align = {"A":4,"F":4,"H":2,"D":8,"C":1,"X":1,"B":1,"P":1,"Z":1}[t]
    if ln is not None: return dup, ln, 1, op
    if t in "CX":
        lit = re.search(r"'([^']*)'", op)
        n = len(lit.group(1)) if lit else 1
        return dup, ((n+1)//2 if t=="X" else n), 1, op
    return dup, {"A":4,"F":4,"H":2,"D":8,"B":1,"P":1,"Z":1}[t], align, op

def layout(path, first):
    off, sym, started = 0, {}, False
    for L in io.open(path, encoding='latin-1').read().split("\n"):
        if L.startswith("*") or L.startswith(".") or not L.strip(): continue
        m = re.match(r"^(\S*)\s+(DC|DS|EQU|ORG)\s+(\S.*?)(?:\s{2,}|$)", L)
        if not m: continue
        label, op, operand = m.group(1), m.group(2), m.group(3)
        if label == first: started = True
        if not started or op == "EQU": continue
        r = operand_size(operand)
        if r is None: continue
        dup, size, align, norm = r
        if align > 1 and off % align: off += align - (off % align)
        if label: sym[label] = off
        if norm.startswith("0"): continue          # DS 0D / DS 0F -- align only
        off += dup * size
    return sym

def check(name, sym, known, derive):
    bad = 0
    print("=== %s : POSITIVE CONTROL (IFOX00-proved offsets) ===" % name)
    for k, v in sorted(known.items(), key=lambda x: x[1]):
        got = sym.get(k); ok = got == v; bad += (not ok)
        print("  %-9s expect %#04x  computed %s  %s" %
              (k, v, ("%#04x" % got) if got is not None else "MISSING",
               "ok" if ok else "*** MISMATCH ***"))
    print("  ---> %d/%d reproduce" % (len(known)-bad, len(known)))
    if bad:
        print("  COMPUTATION WRONG -- derived offsets unusable.\n"); return False
    print("  derived: " + "  ".join("%s=%#04x" % (d, sym[d]) for d in derive) + "\n")
    return True

OUCB_KNOWN = {"OUCBFWD":0x04,"OUCBBCK":0x08,"OUCBQFL":0x10,"OUCBSFL":0x11,
  "OUCBYFL":0x12,"OUCBAFL":0x13,"OUCBTFL":0x14,"OUCBEFL":0x15,"OUCBUFL":0x17,
  "OUCBDMN":0x24,"OUCBSRC":0x25,"OUCBSWC":0x26,"OUCBASCB":0x28,"OUCBACT":0x50,
  "OUCBACN":0x54,"OUCBCFL":0x56,"OUCBNDS":0x84}
ASCB_KNOWN = {"ASCBCPUS":0x20,"ASCBASID":0x24,"ASCBSTOR":0x2C,"ASCBEJST":0x40,
  "ASCBRCTF":0x66,"ASCBFLG1":0x67,"ASCBASXB":0x6C,"ASCBSWCT":0x70,
  "ASCBDSP1":0x72,"ASCBOUCB":0x90,"ASCBOUXB":0x94,"ASCBJBNI":0xAC,
  "ASCBJBNS":0xB0}
d = sys.argv[1]
ok1 = check("IRAOUCB", layout(d+"/IRAOUCB.txt","OUCBNAME"), OUCB_KNOWN,
            ["OUCBPSO","OUCBWSS"])
ok2 = check("IHAASCB", layout(d+"/IHAASCB.txt","ASCBASCB"), ASCB_KNOWN,
            ["ASCBFMCT"])
sys.exit(0 if (ok1 and ok2) else 1)
