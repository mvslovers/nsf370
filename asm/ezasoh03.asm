         TITLE 'EZASOH03 - EZASMI/EZASOKET BACKEND FACADE (THIN VENEER)'
*
*  EZASOH03 - the call-compatible replacement for IBM's EZASOKET backend
*            module (extended return code 10110 = "LOAD of EZASOH03
*            failed" pins EZASOH03 as IBM's OWN module name -- ADR-0029).
*            Shelby Beach's EZASMI macro routes ALL socket traffic through
*            this single entry point; swapping this module puts the whole
*            existing EZASMI ecosystem on NSF without touching a macro.
*
*  A THIN VENEER (ADR-0029, "no socket logic in the facade").  It does NOT
*  decode anything: it hands its R1 plist straight to the C decoder
*  nsf_ezasoh03 (@@NSOH03, src/nsfeza.c), which owns EVERY function-code
*  decode, plist marshalling and RETCODE/ERRNO store -- all in portable,
*  host-tested C (test/host/tsteza.c).  The only MVS-only-unverified code
*  is the ~6-instruction linkage below; test/mvs/tstezah.asm is its
*  Stage-0 live probe (two consecutive calls + two subtasks -- a corrupt
*  save area shows up on the SECOND call, not the first).
*
*  ABI (pinned from Shelby's source in ADR-0029 / conformance doc 2):
*    entry EZASOH03, invoked  L R15,=V(EZASOH03); BALR R14,R15  with
*      R1 -> plist:  +0 A(4-char EBCDIC function code)
*                    +4 A(ERRNO)      +8 A(RETCODE)
*                    +12.. A(function-specific parameters)
*    return via R14; R15 IS ALWAYS 0 (real errors live in RETCODE/ERRNO,
*    never R15).
*
*  ENTRY CONVENTION -- why PDPPRLG, NOT FUNHEAD (the crux; primary-source):
*    This routine CALLS a cc370 C function.  The cc370 C prologue
*    (maclib/pdpprlg.macro) allocates the callee's DSA from the CALLER's
*    DSANAB at 76(R13) -- so to call C we must present a valid cc370 DSA,
*    not a bare OS save area.  FUNHEAD (mvsmacs.macro) never sets DSANAB;
*    a C call after it reads a garbage NAB and corrupts the save chain --
*    exactly the S0C6-on-the-next-call class (CLAUDE.md 3, issue #8).
*    PDPPRLG is the PROVEN pattern for a hand-written socket-API asm entry
*    that calls C in THIS ecosystem: libc370's dyn75 socket() entry
*    (src/dyn75/@@75sock.s) is EZASOH03's exact analog and uses PDPPRLG +
*    an arglist at 88(13) + L R15,=V(func); BALR.  PDPPRLG also gives a
*    PER-INVOCATION DSA off each caller's C stack -- concurrency-safe for
*    the nebenlaeufig app subtasks that enter here, with no static save
*    area (the static-area corruption Mike flagged) and no GETMAIN.  The
*    M3-4 caller is always in C context (tstezah's C driver, tstezam, a
*    relinked C app); a pure-asm EZASMI caller with no C environment is the
*    M6 relink audit's problem and hooks in HERE later without touching the
*    decoder (ADR-0029).
*
*  AS370 discipline (CLAUDE.md 3): every statement inside column 71 (col 72
*  is a continuation flag); RS-format operands stay D(B); modelled verbatim
*  on the proven @@75sock.s structure.
*
         COPY  PDPTOP
         CSECT ,
*
&FUNC    SETC  'EZASOH03'
         DS    0F
* X-func EZASOH03 prologue (the cc370 C prologue -- see the note above)
EZASOH03 PDPPRLG CINDEX=0,FRAME=96,BASER=12,ENTRY=YES
         B     @@FEN0
         LTORG
@@FEN0   EQU   *
         DROP  12
         BALR  12,0
         USING *,12
* Function EZASOH03 code: pass R1 (the plist) as the sole C arg to the decoder
         LR    11,1               R11 = incoming R1 = the EZASOH03 plist
         ST    11,88(,13)         C arg list[0] = plist
         LA    1,88(,13)          R1 -> the one-word C arg list
         L     15,=V(@@NSOH03)    the C decoder owns all the logic
         BALR  14,15
         SLR   15,15              R15 = 0 ALWAYS (the EZASOH03 ABI)
* Function EZASOH03 epilogue
         PDPEPIL
         DS    0F
         LTORG
         END
