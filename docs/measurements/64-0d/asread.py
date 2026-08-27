#!/usr/bin/env python3
"""64-0d: SRM + task state of ANY address space, read live and read-only.

Keyed by ASCB, not by anchor, because 3.1(2) needs the same reading for the
parked clients and for a healthy third address space (HTTPD).

Identity, never coherence (the 64-0c /.dm trap): LSQA is aliased at the SAME
virtual address in every address space, so a chain that looks perfect can be
the wrong space.  Two independent identity proofs are asserted here:
  * OUCBASCB inside the OUCB must equal the ASCB we chased from;
  * every private read goes through THAT ASCB's OWN ASCBSTOR segment table.
The page map is built FRESH each run and never cached to disk (a cached map
reads a swapped-out page as stale-but-coherent -- the same trap one level down).

Offsets: CBOFF (JOB02215) / CBOFF2 (JOB02217) / CBOFF3 / CBOFF4 (JOB02231)
and CBOFF5 (JOB02253, this round) -- all IFOX00 over SYS1.AMODGEN, zero
diagnostics.  DAT constants from the running Hercules build's esa390.h.
"""
import base64, os, re, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import herc

HOST = "mvsdev.lan:8080"

# ---- CBOFF/CBOFF2/CBOFF3/CBOFF4 -----------------------------------------
ASCBASID, ASCBSTOR, ASCBASXB, ASCBCPUS = 0x24, 0x2C, 0x6C, 0x20
ASCBRCTF, ASCBFLG1, ASCBDSP1, ASCBSWCT, ASCBEJST = 0x66, 0x67, 0x72, 0x70, 0x40
ASXBFTCB, ASXBLTCB, ASXBTCBS = 0x04, 0x08, 0x0C
TCBRBP, TCBTCB, TCBOTC, TCBFLGS, TCBFLGS4, TCBFLGS5, TCBPKF = \
    0x00, 0x74, 0x84, 0x1D, 0x20, 0x21, 0x1C
TCBSCNDY = 0xAC
RBFTP_MASK, RBXWAIT, RBLONGWT, RBECBWT, RBTCBNXT = 0xE0, 0x40, 0x04, 0x01, 0x80
RBFTP = {0x00: "PRB", 0xC0: "SVRB", 0x40: "IRB", 0x80: "SIRB", 0x60: "TIRB"}
# ---- CBOFF5 (this round) -------------------------------------------------
ASCBOUCB, ASCBOUXB = 0x90, 0x94
OUCBFWD, OUCBBCK, OUCBQFL, OUCBSFL, OUCBYFL, OUCBAFL = 0x04, 0x08, 0x10, 0x11, 0x12, 0x13
OUCBTFL, OUCBEFL, OUCBUFL, OUCBDMN, OUCBSRC, OUCBSWC = 0x14, 0x15, 0x17, 0x24, 0x25, 0x26
OUCBASCB, OUCBACT, OUCBACN, OUCBCFL, OUCBNDS, OUCBLEN = 0x28, 0x50, 0x54, 0x56, 0x84, 0x90
OUXBRSW = 0x0C
BITS_RCTF = [(0x80,"TMNO"),(0x40,"FRS"),(0x20,"FQU"),(0x08,"WAIT"),
             (0x04,"OUT"),(0x02,"TMLW")]
BITS_FLG1 = [(0x10,"TERM"),(0x08,"ABNT"),(0x04,"STND"),(0x02,"TYP1"),(0x01,"NSWP")]
BITS_DSP1 = [(0x80,"NOQ"),(0x40,"FAIL")]
BITS_QFL  = [(0x80,"GOO"),(0x40,"GOI"),(0x20,"GOB"),(0x08,"OFF"),(0x04,"OUT")]
BITS_SFL  = [(0x80,"NSW"),(0x10,"INV"),(0x08,"NSWI")]
BITS_EFL  = [(0x80,"LWT"),(0x40,"TRM"),(0x20,"OWT"),(0x01,"MWT")]
BITS_UFL  = [(0x20,"RSWP"),(0x10,"TSWP"),(0x08,"TSWC")]
BITS_CFL  = [(0x80,"RDY"),(0x40,"RSM"),(0x20,"DFSW")]

def dec(v, tab): return ",".join(n for m, n in tab if v & m) or "-"
def w(b, o): return int.from_bytes(b[o:o+4], "big")
def h(b, o): return int.from_bytes(b[o:o+2], "big")
def a24(b, o): return int.from_bytes(b[o+1:o+4], "big")

class Reader:
    def __init__(self, user, pasw):
        self._auth = base64.b64encode(("%s:%s" % (user, pasw)).encode()).decode()
        self.pmap = {}          # (stor, page) -> real frame ; in-process only
    def dm(self, addr, n):
        url = "http://%s/.dm?m=%X&l=%d&c=16" % (HOST, addr, n)
        rq = urllib.request.Request(url, headers={"Authorization": "Basic " + self._auth})
        body = re.sub(r"<[^>]*>", "",
                      urllib.request.urlopen(rq, timeout=25).read().decode("latin-1"))
        out = bytearray()
        for line in body.split("\n"):
            m = re.match(r"\s*\+[0-9A-F]{5}\s+((?:[0-9A-F]{8}\s*){1,4})", line)
            if m: out += bytes.fromhex(m.group(1).replace(" ", ""))
        return bytes(out[:n])
    # ---- DAT, built fresh, per address space -----------------------------
    def map_pages(self, stor, vaddrs):
        need = {v & 0xFFFFF000 for v in vaddrs}
        need = {p for p in need if (stor, p) not in self.pmap}
        if not need: return
        sto = stor & herc.STD_370_STO
        segs = sorted({p >> 16 for p in need})
        t = herc.herc(sorted({"r %X.10" % ((sto + s*4) & ~0xF) for s in segs}))
        segmem = herc.parse_real(t)
        ptos = {}
        for s in segs:
            ste = herc.flatten(segmem, sto + s*4, 4)
            if ste is None: continue
            ste = int.from_bytes(ste, "big")
            if not (ste & herc.SEGTAB_370_INVL):
                ptos[s] = ste & herc.SEGTAB_370_PTO
        if not ptos: return
        t = herc.herc(sorted({"r %X.40" % (p & ~0xF) for p in ptos.values()}))
        pagemem = herc.parse_real(t)
        for p in need:
            pto = ptos.get(p >> 16)
            if pto is None: continue
            pte = herc.flatten(pagemem, pto + (((p >> 12) & 0xF) * 2), 2)
            if pte is None: continue
            pte = int.from_bytes(pte, "big")
            if pte & herc.PAGETAB_INV_4K: continue
            self.pmap[(stor, p)] = (pte & herc.PAGETAB_PFRA_4K) << 8
    def real(self, stor, v):
        f = self.pmap.get((stor, v & 0xFFFFF000))
        return None if f is None else f | (v & 0xFFF)
    def fetch(self, stor, specs):
        """specs = [(vaddr, len)] -> {vaddr: bytes|None}, private storage."""
        self.map_pages(stor, [v for v, _ in specs])
        cmds, plan = [], []
        for v, n in specs:
            r = self.real(stor, v)
            if r is None: plan.append((v, n, None)); continue
            lo = r & ~0xF; ln = ((r + n + 15) & ~0xF) - lo
            cmds.append("r %X.%X" % (lo, ln)); plan.append((v, n, r))
        mem = herc.parse_real(herc.herc(sorted(set(cmds)))) if cmds else {}
        return {v: (herc.flatten(mem, r, n) if r is not None else None)
                for v, n, r in plan}

def read_as(rd, ascb, tag, tcbs=True):
    print("=== %s  [%s]  ASCB=%06X ==="
          % (time.strftime("%Y-%m-%d %H:%M:%S %Z"), tag, ascb), flush=True)
    a = rd.dm(ascb, 0xB4)
    if a[0:4] != b"\xC1\xE2\xC3\xC2":            # 'ASCB' in EBCDIC
        print("  ASCB eyecatcher MISSING -- refusing to read on"); return None
    stor, asxbv, asid = w(a, ASCBSTOR), w(a, ASCBASXB), h(a, ASCBASID)
    oucbp, ouxbp = w(a, ASCBOUCB), w(a, ASCBOUXB)
    print("  ASID=%04X CPUS=%d EJST=%08X%08X SWCT=%d ASCBSTOR=%08X"
          % (asid, w(a, ASCBCPUS), w(a, ASCBEJST), w(a, ASCBEJST+4),
             h(a, ASCBSWCT), stor))
    print("  RCTF=%02X[%s]  FLG1=%02X[%s]  DSP1=%02X[%s]"
          % (a[ASCBRCTF], dec(a[ASCBRCTF], BITS_RCTF),
             a[ASCBFLG1], dec(a[ASCBFLG1], BITS_FLG1),
             a[ASCBDSP1], dec(a[ASCBDSP1], BITS_DSP1)))
    # ---- OUCB.  Identity proof: OUCBASCB must name the ASCB we came from.
    if oucbp:
        u = rd.dm(oucbp, OUCBLEN)
        ok = (u[0:4] == b"\xD6\xE4\xC3\xC2") and (w(u, OUCBASCB) == ascb)
        print("  OUCB  @%06X eye=%s OUCBASCB=%06X %s"
              % (oucbp, u[0:4].decode("cp037", "replace"), w(u, OUCBASCB),
                 "IDENTITY OK" if ok else "*** IDENTITY MISMATCH -- NOT USED ***"))
        if ok:
            print("        QFL=%02X[%s] SFL=%02X[%s] EFL=%02X[%s] UFL=%02X[%s]"
                  " CFL=%02X[%s]"
                  % (u[OUCBQFL], dec(u[OUCBQFL], BITS_QFL),
                     u[OUCBSFL], dec(u[OUCBSFL], BITS_SFL),
                     u[OUCBEFL], dec(u[OUCBEFL], BITS_EFL),
                     u[OUCBUFL], dec(u[OUCBUFL], BITS_UFL),
                     u[OUCBCFL], dec(u[OUCBCFL], BITS_CFL)))
            print("        SRC(swapout reason)=%02X SWC=%d NDS(dontswaps)=%d"
                  " DMN=%d YFL=%02X AFL=%02X TFL=%02X"
                  % (u[OUCBSRC], h(u, OUCBSWC), h(u, OUCBNDS), u[OUCBDMN],
                     u[OUCBYFL], u[OUCBAFL], u[OUCBTFL]))
            print("        FWD=%06X BCK=%06X ACT(action q)=%06X ACN=%04X"
                  % (w(u, OUCBFWD), w(u, OUCBBCK), w(u, OUCBACT), h(u, OUCBACN)))
    if ouxbp:
        x = rd.fetch(stor, [(ouxbp, 0x10)])[ouxbp]
        if x is None:
            print("  OUXB  @%06X : page not mapped -- NOT READ" % ouxbp)
        else:
            print("  OUXB  @%06X eye=%s OUXBRSW(REQSWAP ecb)=%08X"
                  % (ouxbp, x[0:4].decode("cp037", "replace"), w(x, OUXBRSW)))
    if not tcbs: return stor
    # ---- ASXB / TCB / RB, all through THIS address space's own tables ----
    asxb = rd.fetch(stor, [(asxbv, 16)])[asxbv]
    if asxb is None or asxb[0:4] != b"\xC1\xE2\xE7\xC2":
        print("  ASXB unreadable at virt %06X" % asxbv); return stor
    ftcb, ltcb, ntcb = w(asxb, ASXBFTCB), w(asxb, ASXBLTCB), h(asxb, ASXBTCBS)
    print("  ASXB  virt=%06X real=%06X FTCB=%06X LTCB=%06X TCBS=%d"
          % (asxbv, rd.real(stor, asxbv), ftcb, ltcb, ntcb))
    chain, t = [], ftcb
    while t and len(chain) < 24:
        blk = rd.fetch(stor, [(t, 0xF0)])[t]
        if blk is None: print("   TCB @%06X unreadable" % t); break
        chain.append((t, blk)); t = w(blk, TCBTCB)
    rbs = rd.fetch(stor, [(w(b, TCBRBP) - 8, 0x30) for _, b in chain if w(b, TCBRBP)])
    for i, (tv, b) in enumerate(chain):
        print("   TCB%-2d %06X PKF=%02X FLGS5=%02X SCNDY=%s OTC=%06X RBP=%06X"
              % (i, tv, b[TCBPKF], b[TCBFLGS5],
                 b[TCBSCNDY:TCBSCNDY+4].hex().upper(), a24(b, TCBOTC),
                 w(b, TCBRBP)))
        r = rbs.get(w(b, TCBRBP) - 8)
        if r is None: print("         RB unreadable"); continue
        f1, intcod = r[0], h(r, 6)
        stab1, stab2, wcf = r[8+0x0A], r[8+0x0B], r[8+0x1C]
        print("         RB=%06X TYPE=%-4s WCF=%d INTCOD=%04X(SVC %d)"
              " FLAGS1=%02X[%s%s] STAB2=%02X[%s%s] CDE=%06X"
              % (w(b, TCBRBP), RBFTP.get(stab1 & RBFTP_MASK, "?"), wcf, intcod,
                 intcod & 0xFF, f1, "XWAIT " if f1 & RBXWAIT else "",
                 "LONGWT" if f1 & RBLONGWT else "", stab2,
                 "ECBWT " if stab2 & RBECBWT else "",
                 "TCBNXT" if stab2 & RBTCBNXT else "", a24(r, 8+0x0C)))
        print("         RBOPSW=%08X %08X" % (w(r, 8+0x10), w(r, 8+0x14)))
    return stor

if __name__ == "__main__":
    user, pasw = sys.argv[1], sys.argv[2]
    rd = Reader(user, pasw)
    for spec in sys.argv[3:]:
        name, _, addr = spec.partition("=")
        read_as(rd, int(addr, 16), name)
        print(flush=True)
