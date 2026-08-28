#!/usr/bin/env python3
# 40-CHK: read the NSFS CSA anchor header and one slot through HTTPD /.dm.
# READ-ONLY. Offsets from include/nsfvsvc.h (NSFV_OFF_ASSERT-pinned).
import re, subprocess, sys, datetime

ANCHOR = int(sys.argv[1], 16)
SLOT   = int(sys.argv[2]) if len(sys.argv) > 2 else 0
HDR, STRIDE = 56, 2144
STATE = {0:"FREE",1:"PENDING",2:"DONE",3:"HELD",4:"CLAIMED"}

def dm(addr, ln):
    url = f"http://mvsdev.lan:8080/.dm?m={addr:08X}&l={ln}"
    out = subprocess.run(["curl","-s","-u","IBMUSER:SYS1",url],
                         capture_output=True, text=True).stdout
    words = []
    for line in out.splitlines():
        if not line.startswith("+"): continue
        for w in re.findall(r'>([0-9A-F]{8})</a>', line):
            words.append(int(w, 16))
    return words

ts = datetime.datetime.now(datetime.timezone.utc).strftime("%H:%M:%SZ")
h = dm(ANCHOR, 56)
print(f"[{ts}] ANCHOR {ANCHOR:08X}  eye={bytes.fromhex('%08X%08X'%(h[0],h[1])).decode('cp037')}"
      f" ver={h[2]} flags={h[3]:08X} inflight={h[6]} served={h[7]} reaped={h[8]}"
      f" nslots={h[10]} exhausted={h[11]} collisions={h[12]}")
a = ANCHOR + HDR + SLOT*STRIDE
s = dm(a, 32)
print(f"[{ts}] SLOT {SLOT} @{a:08X}  req_state={s[0]} ({STATE.get(s[0],'?')})"
      f" req_token={s[1]:08X} reply_ecb={s[2]:08X} (&={a+8:08X})"
      f" req_ascb={s[3]:08X} req_asid={s[4]:04X} xfunc={s[5]} xlen={s[6]}")
