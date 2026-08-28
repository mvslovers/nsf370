         TITLE 'NSFSEVT - SYSEVENT (SVC 95) SEAM'
*
*  NSFSEVT - the one C-callable entry through which NSF issues
*  SYSEVENT:
*
*    nsf_sysevent(UINT code) -> INT   -- R0 = code, SVC 95, R15 back
*
*  DERIVED FROM PRIMARY SOURCE, NOT COPIED. The expansion below is what
*  SYS1.AMODGEN(SYSEVENT) generates for the no-ASID / ENTRY=SVC case,
*  read
*  live off the target (64-3-0, docs/nsf-64-3-0-noswap-survey.md
*  2.2-2.3):
*
*    .OKLAB   LA  0,&EVENTO(0,0)   SYSEVENT CODE      (macro line ~170)
*    .SVC     SVC 95               SYSTEM RESOURCES MANAGER SVC  (line
*    209)
*
*  With no ASID= / ASIDL= / PGN= the macro's .SHFTID shift is NOT
*  taken, so
*  R0 is the bare event code. as370 has no SYSEVENT macro (checked:
*  MVSMACS
*  and pdptop.copy define none), so the two instructions are written
*  out.
*  The event CODE is the caller's business -- see include/nsfswap.h.
*
*  PRECEDENT, NOT SOURCE: mvs38j-ip's mvsasm/mvsevent.asm is the reason
*  we
*  know this works on 3.8j. Its code is NOT reused (ADR-0005). Its two
*  operational facts are: SYSEVENT requires KEY 0 (its SPKA 0, line
*  89), and
*  R15 is UNSPECIFIED -- "The doc I have doesn't indicate a return
*  code"
*  (lines 15-16). We return R15 for the record and never branch on it.
*
*  STATE: the caller supplies supervisor state and key 0 via libc370
*  __super(PSWKEY0, ...) -- which does MODESET MODE=SUP *and* SPKA --
*  so this
*  routine deliberately contains NO key manipulation of its own. That
*  keeps
*  "what executes under a borrowed key" one short block in nsfswap.c
*  rather
*  than two, and matches the 12 existing __super sites in nsfsx.c.
*
*  ENTRY CONVENTION (issue #8): standard cc370 -- COPY MVSMACS + COPY
*  PDPTOP,
*  FUNHEAD / FUNEXIT. The FUNHEAD entry name IS the 8-char asm() alias.
*
*  LEAF FORM IS CORRECT HERE, and the reason is the SVC TYPE, not the
*  fact
*  that an SVC is issued. NSFCIHLT uses FUNHEAD SAVE= because SVC 33 is
*  TYPE 2 -- it runs in an SVRB and needs proper save-area linkage. SVC
*  95
*  is TYPE 1, MEASURED from this target's live SVCTABLE entry in 64-3-0
*  (type1, svcapf off): a type 1 SVC saves into the SVRB the FLIH owns
*  and
*  never walks the caller's R13 chain. That is also why nsftmr_plat_arm
*  issues STIMER (SVC 47, type 1) under the plain leaf form and is
*  correct.
*
*  AS370 QUIRKS in force: keep every statement inside column 71 (column
*  72
*  is a continuation flag and a long comment silently swallows the NEXT
*  instruction -- M3-0b, M3-4 and M5-2b2 each paid for this); RS-format
*  operands stay D(B); no bare-label USING.
*
         COPY  MVSMACS
         COPY  PDPTOP
         CSECT ,
         PRINT GEN
*---------------------------------------------------------------------*
*  nsf_sysevent(UINT code) -> INT                                     *
*    R1 -> arg vector; arg0 = the SYSEVENT code (e.g. DONTSWAP 41).   *
*    Caller is already supervisor state / key 0.                      *
*---------------------------------------------------------------------*
NSFSEVT  FUNHEAD ,                issue SYSEVENT
         L     R0,0(,R1)          R0 = event code (arg0)
         SVC   95                 SYSEVENT -- SRM
         FUNEXIT RC=(R15)
         END
