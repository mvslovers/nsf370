"""Read REAL storage through the Hercules console (tmux) -- read-only.

`r addr.len` needs only an online CPU (hscemode.c abs_or_r_cmd): no CPU stop,
no savecore, nothing altered.  Commands are batched into one ssh round trip and
the replies are parsed out of ~/MVSCE/hercules.log.
"""
import re, subprocess

def herc(cmds, settle=2):
    """Run Hercules console commands; return the new hercules.log lines."""
    script = ["wc -l < ~/MVSCE/hercules.log > /tmp/hlog.mark"]
    for c in cmds:
        script.append("tmux send-keys -t 0:0.0 '%s' Enter" % c)
        script.append("sleep 0.4")
    script.append("sleep %d" % settle)
    script.append("tail -n +$(( $(cat /tmp/hlog.mark) + 1 )) ~/MVSCE/hercules.log")
    out = subprocess.run(["ssh", "mvsdev", "\n".join(script)],
                         capture_output=True, text=True, timeout=180)
    return out.stdout

_RE = re.compile(r"HHC02290I R:([0-9A-F]{8,16})\s+((?:[0-9A-F]{8} ?){1,4})")

def parse_real(text):
    """-> {real_address: bytes} from every 'r' reply in the text."""
    mem = {}
    for m in _RE.finditer(text):
        addr = int(m.group(1), 16)
        mem[addr] = bytes.fromhex(m.group(2).replace(" ", ""))
    return mem

def flatten(mem, addr, n):
    """Assemble n bytes at addr from a parsed {addr: bytes} map."""
    out = bytearray()
    a = addr
    while len(out) < n:
        base = a & ~0xF
        chunk = mem.get(base)
        if chunk is None:
            return None
        off = a - base
        take = min(n - len(out), len(chunk) - off)
        if take <= 0:
            return None
        out += chunk[off:off+take]
        a += take
    return bytes(out)

# ---- S/370 DAT, formats from Hercules esa390.h (this exact build) ----------
STD_370_STO   = 0x00FFFFC0
SEGTAB_370_PTO  = 0x00FFFFF8
SEGTAB_370_INVL = 0x00000001
PAGETAB_PFRA_4K = 0xFFF0
PAGETAB_INV_4K  = 0x0008

def dat_plan(stor, vaddrs):
    """Addresses whose REAL contents are needed to translate vaddrs (2 passes)."""
    sto = stor & STD_370_STO
    return sorted({sto + ((v >> 16) * 4) for v in vaddrs})

def dat_translate(stor, vaddr, segmem, pagemem):
    sto = stor & STD_370_STO
    ste = flatten(segmem, sto + ((vaddr >> 16) * 4), 4)
    if ste is None: return None, "STE unreadable"
    ste = int.from_bytes(ste, "big")
    if ste & SEGTAB_370_INVL: return None, "segment invalid (STE=%08X)" % ste
    pto = ste & SEGTAB_370_PTO
    pte = flatten(pagemem, pto + (((vaddr >> 12) & 0xF) * 2), 2)
    if pte is None: return None, "PTE unreadable"
    pte = int.from_bytes(pte, "big")
    if pte & PAGETAB_INV_4K: return None, "page invalid (PTE=%04X)" % pte
    return ((pte & PAGETAB_PFRA_4K) << 8) | (vaddr & 0xFFF), None
