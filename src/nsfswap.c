/*
 * nsfswap.c -- SRM swappability control for the Phase-2 STC (issue #64).
 *
 * Phase 2 ONLY: this file appears in the NSFS [[module]] source list and not
 * in NSF's. See include/nsfswap.h for why, and for where every code and
 * offset below was read from.
 *
 * WHAT IS PROVEN BY WHAT. R15 from SVC 95 is NOT the proof -- accepted,
 * rejected and SILENTLY IGNORED are three outcomes and the third is this
 * project's standing failure class (CLAUDE.md 8.5). The proof is reading
 * OUCBNSW / OUCBNDS / OUCBASW back afterwards, through ONE code path with
 * ONE identity assertion so the baseline and the post-call reading are
 * comparable. Nothing in this file branches on R15.
 */

#include "nsfswap.h"
#include "nsfmsg.h"

#include <clibos.h>            /* __ascb, __super, __prob, PSWKEY0 */
#include <string.h>            /* memset, memcmp */

/* -- the reading ----------------------------------------------------------- */

INT nsfswap_read(NSFSWAPVIEW *v)
{
    const UCHAR *ascb;
    const UCHAR *oucb;
    UINT         oucbaddr;
    UINT         backptr;

    if (v == NULL) {
        return 1;
    }
    memset(v, 0, sizeof(*v));

    ascb = (const UCHAR *)__ascb(0u);
    if (ascb == NULL) {
        return 2;
    }
    /* Eyecatchers are compared against character literals, never byte values,
     * so the same source is right on an EBCDIC target and an ASCII host
     * (spec 15.3 -- the charset-transparent rule). */
    if (memcmp(ascb, "ASCB", 4) != 0) {
        return 3;
    }

    oucbaddr = *(const UINT *)(ascb + NSFSWAP_ASCBOUCB) & 0x00FFFFFFu;
    if (oucbaddr == 0u) {
        return 4;
    }
    oucb = (const UCHAR *)oucbaddr;
    if (memcmp(oucb, "OUCB", 4) != 0) {
        return 5;
    }
    /* IDENTITY, not coherence: the OUCB must point back at the ASCB we chased
     * it from. Without this a plausible-looking chain can describe another
     * address space (the 64-0c trap). */
    backptr = *(const UINT *)(oucb + NSFSWAP_OUCBASCB) & 0x00FFFFFFu;
    if (backptr != ((UINT)ascb & 0x00FFFFFFu)) {
        return 6;
    }

    v->oucb = oucbaddr;
    v->nds  = *(const USHORT *)(oucb + NSFSWAP_OUCBNDS);
    v->fmct = *(const USHORT *)(ascb + NSFSWAP_ASCBFMCT);
    v->sfl  = oucb[NSFSWAP_OUCBSFL];
    v->afl  = oucb[NSFSWAP_OUCBAFL];
    v->qfl  = oucb[NSFSWAP_OUCBQFL];
    return 0;
}

/* -- reporting ------------------------------------------------------------- */

static void swap_report(const char *tag, const NSFSWAPVIEW *v, INT rc)
{
    if (rc != 0) {
        nsfmsg("NSF840E %s OUCB UNREADABLE -- IDENTITY RC=%d", tag, (int)rc);
        return;
    }
    nsfmsg("NSF841I %s OUCB=%06X NSW=%c ASW=%c GOO=%c NDS=%u FMCT=%u",
           tag, (unsigned)v->oucb,
           (v->sfl & NSFSWAP_NSW) ? 'Y' : 'N',
           (v->afl & NSFSWAP_ASW) ? 'Y' : 'N',
           (v->qfl & NSFSWAP_GOO) ? 'Y' : 'N',
           (unsigned)v->nds, (unsigned)v->fmct);
    nsfmsg("NSF842I %s RAW SFL=%02X AFL=%02X QFL=%02X",
           tag, (unsigned)v->sfl, (unsigned)v->afl, (unsigned)v->qfl);
}

/* -- Stage A: the probe ---------------------------------------------------- */

void nsfswap_probe(void)
{
    NSFSWAPVIEW   base;
    NSFSWAPVIEW   afteron;
    NSFSWAPVIEW   afteroff;
    unsigned char savekey;
    INT           rcbase;
    INT           rcon;
    INT           rcoff;
    INT           r15on  = 0;
    INT           r15off = 0;

    /* One key-0 window for the whole sequence: the reads do not need it (SQA
     * is not fetch-protected), the SVC does. Nothing is WTOed from inside it
     * -- the readings are reported after __prob. */
    if (__super(PSWKEY0, &savekey) != 0) {
        nsfmsg("NSF843E SWAP PROBE: CANNOT ENTER SUPERVISOR/KEY 0");
        return;
    }

    rcbase = nsfswap_read(&base);
    if (rcbase == 0) {
        r15on = nsf_sysevent(NSFSWAP_DONTSWAP);
        rcon  = nsfswap_read(&afteron);
        r15off = nsf_sysevent(NSFSWAP_OKSWAP);
        rcoff  = nsfswap_read(&afteroff);
    } else {
        rcon  = rcbase;
        rcoff = rcbase;
    }

    __prob(savekey, NULL);

    nsfmsg("NSF844I SWAP PROBE (64-3-1 STAGE A) -- DONTSWAP=%u OKSWAP=%u"
           " VIA SVC 95", NSFSWAP_DONTSWAP, NSFSWAP_OKSWAP);
    swap_report("1 BASELINE", &base, rcbase);
    if (rcbase != 0) {
        nsfmsg("NSF845E SWAP PROBE ABANDONED -- NO IDENTITY, NOTHING ISSUED");
        return;
    }
    nsfmsg("NSF846I 2 DONTSWAP ISSUED, R15=%d (UNSPECIFIED -- NOT THE PROOF)",
           (int)r15on);
    swap_report("3 AFTER DONTSWAP", &afteron, rcon);
    nsfmsg("NSF846I 4 OKSWAP ISSUED, R15=%d (UNSPECIFIED -- NOT THE PROOF)",
           (int)r15off);
    swap_report("5 AFTER OKSWAP", &afteroff, rcoff);

    /* The verdict is the read-back, and step 5 is not tidy-up: a probe that
     * leaves NSFS pinned has changed the machine as a side effect of
     * measuring it. OUCBNDS is a COUNT -- compare against the baseline
     * VALUE, which is not assumed to be zero. */
    if (rcon == 0 && rcoff == 0) {
        if (afteron.nds > base.nds && (afteron.sfl & NSFSWAP_NSW) != 0u) {
            nsfmsg("NSF847I VERDICT: DONTSWAP ACCEPTED"
                   " (NDS %u->%u, NSW SET)",
                   (unsigned)base.nds, (unsigned)afteron.nds);
        } else {
            nsfmsg("NSF848W VERDICT: DONTSWAP DID NOT TAKE"
                   " (NDS %u->%u, NSW %c) -- TREAT AS IGNORED",
                   (unsigned)base.nds, (unsigned)afteron.nds,
                   (afteron.sfl & NSFSWAP_NSW) ? 'Y' : 'N');
        }
        if (afteroff.nds == base.nds) {
            nsfmsg("NSF849I RELEASE PROVEN: NDS BACK AT BASELINE %u",
                   (unsigned)base.nds);
        } else {
            nsfmsg("NSF850E RELEASE FAILED: NDS %u, BASELINE WAS %u"
                   " -- ADDRESS SPACE LEFT CHANGED",
                   (unsigned)afteroff.nds, (unsigned)base.nds);
        }
    }
}

/* -- the operator verb ----------------------------------------------------- */

void nsfswap_op(const char *arg)
{
    (void)arg;                  /* Stage A takes no operand */
    nsfswap_probe();
}
