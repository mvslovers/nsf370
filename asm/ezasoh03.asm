         TITLE 'EZASOH03 - EZASMI/EZASOKET BACKEND FACADE VENEER'
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
*    PDPPRLG is the PROVEN pattern for a HAND-WRITTEN asm entry that calls C
*    in THIS ecosystem: libc370's VSAM exit stubs (src/clib/@@vsopen.c, the
*    EODAD/LERAD/SYNAD routines in file-scope __asm__) are EZASOH03's analog
*    -- hand-written asm, entered by a NON-C caller (the VSAM access method,
*    as EZASOH03 is entered by the EZASMI macro), using PDPPRLG + L R15,=V(
*    @@VSXEOF); BALR to call C.  PDPPRLG also gives a
*    PER-INVOCATION DSA off each caller's C stack -- concurrency-safe for
*    the nebenlaeufig app subtasks that enter here, with no static save
*    area (the static-area corruption Mike flagged) and no GETMAIN.  The
*    M3-4 caller is always in C context (tstezah's C driver, tstezam, a
*    relinked C app); a pure-asm EZASMI caller with no C environment is the
*    M6 relink audit's problem and hooks in HERE later without touching the
*    decoder (ADR-0029).
*
*  AS370 discipline (CLAUDE.md 3): every statement inside column 71 (col 72
*  is a continuation flag -- the M3-4 live S0C4: an over-long comment on the
*  LR line swallowed the next LA); RS-format operands stay D(B).
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
*  Body: R11 = incoming R1 = the EZASOH03 plist. Build a one-word C arg
*  list at 88(13) holding A(plist), point R1 at it, and call the C decoder
*  @@NSOH03 (which owns all the logic). Then force R15 = 0 (the EZASOH03
*  ABI: real errors live in RETCODE/ERRNO, never R15).
*  NB CLAUDE.md 3: keep these instruction lines SHORT -- an inline comment
*  reaching column 72 makes as370 treat the NEXT line as a continuation and
*  silently drop it (the M3-4 live S0C4: a 72-col LR line swallowed the LA).
         LR    11,1
         ST    11,88(,13)
         LA    1,88(,13)
         L     15,=V(@@NSOH03)
         BALR  14,15
         SLR   15,15
* Function EZASOH03 epilogue
         PDPEPIL
         DS    0F
         LTORG
         END
