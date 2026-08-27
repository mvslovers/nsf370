#!/usr/bin/env python3
"""64-0f: segment swapwatch.log by arm and report observable 2 per arm.

The reported number is the count of DISTINCT ASCBSTOR values seen inside each
arm's wall-clock window:  1 = no completed swap cycle observed at this cadence,
>= 2 = at least one completed.  Also reports QFL/RCTF/CPUS distributions and
the badid count (identity failures, which are excluded from the distinct count).

usage: summarise.py <swapwatch.log> <arm-name> <HH:MM:SS start> <HH:MM:SS end> [...]
"""
import re, sys

log = open(sys.argv[1]).read().split("\n")
args = sys.argv[2:]
arms = [(args[i], args[i+1], args[i+2]) for i in range(0, len(args), 3)]

def t(s): 
    h, m, sec = s.split(":"); return int(h)*3600 + int(m)*60 + int(sec)

for name, a, b in arms:
    lo, hi = t(a), t(b)
    stors, qfl, rctf, cpus, full, fast, bad = {}, {}, {}, {}, 0, 0, 0
    for line in log:
        m = re.match(r"^(\d\d:\d\d:\d\d) ", line)
        if not m or not (lo <= t(m.group(1)) <= hi):
            continue
        if "BADID" in line or "IDENTITY FAILED" in line:
            bad += 1
        ms = re.search(r"STOR=([0-9A-F]{8})", line)
        if ms:
            full += 1
            stors[ms.group(1)] = stors.get(ms.group(1), 0) + 1
            for pat, d in ((r"QFL=([0-9A-F]{2}\[[^\]]*\])", qfl),
                           (r"RCTF=([0-9A-F]{2}\[[^\]]*\])", rctf),
                           (r"CPUS=(\d+)", cpus)):
                mm = re.search(pat, line)
                if mm: d[mm.group(1)] = d.get(mm.group(1), 0) + 1
        md = re.search(r"samples=(\d+)", line)
        if md: fast = max(fast, int(md.group(1)))
    print("%-10s %s-%s  DISTINCT ASCBSTOR = %d  %s" % (name, a, b, len(stors), sorted(stors)))
    print("           full samples=%d  fast samples(cum)=%d  badid=%d" % (full, fast, bad))
    print("           QFL %s | RCTF %s | CPUS %s" % (qfl, rctf, cpus))
