         TITLE 'TSTHALT - M3-0a STAGE 0: IOHALT (SVC 33) / PURGE (SVC 16)'
*
*  TSTHALT -- test-only seam for test/mvs/tsthio.c (M3-0a Stage 0: can a
*            PROBLEM-STATE program halt an outstanding CTCI READ?). Lives
*            under test/asm/, NOT asm/ -- this Stage 0 probe adds zero
*            production surface (CLAUDE.md gate: no changes to src/ or
*            asm/). Modeled on test/asm/tststmw.asm.
*
*            Two C-callable entries, each issuing exactly the SVC its
*            shipped/documented macro expands to. Neither is COPYed from
*            the real macro (SYS1.MACLIB is not on the host as370 SYSLIB
*            search path for a cross-build) -- each is hand-transcribed
*            verbatim from the primary source read off the live target /
*            IBM documentation (see test/mvs/tsthio.c for citations):
*
*    tsthio_halt(ucb)       -- SYS1.MACLIB(IOHALT), the no-OFFSET
*      (HALTCD) expansion: R0 = UCB (common segment) address, R1 =
*      X'00000001' (IOS HALT I/O option), SVC 33. No documented return
*      code -- the probe's OWN observation of the read ECB is the result.
*
*    tsthio_purge(parmlist) -- OS/VS2 SPL: Data Management (GC26-3830-4,
*      Rel 3.8), "PURGE Macro Definition": R1 = &parmlist, SVC 16.
*      Returns R15 (the PURGE return code; meaningful because the caller
*      sets byte 0 bit 7 of its 16-byte parameter list).
*
*  ENTRY CONVENTION (CLAUDE.md 3): COPY MVSMACS + COPY PDPTOP, FUNHEAD /
*  FUNEXIT, modeled on libc370 @@getclk.asm -- never hand-rolled
*  STM/BALR/USING. Both entries issue an SVC that manipulates the I/O
*  supervisor's request-queue/DEB machinery (the same family as
*  OPEN/CLOSE/EXCP, per CLAUDE.md 3's "C-callable HLASM" rule), so both
*  use FUNHEAD SAVE=,US=NO with a static save area, not the plain leaf
*  form -- this is a single-task probe, so one shared static area is
*  safe (no concurrent callers; contrast asm/nsfctcio.asm's per-scb
*  areas, needed only because the M1-4b I/O subtasks call concurrently).
*
         COPY  MVSMACS
         COPY  PDPTOP
         CSECT ,
         PRINT GEN
*---------------------------------------------------------------------*
*  tsthio_halt(ucb) -> void                                           *
*     R0 = ucb (the C argument's value), R1 = X'00000001', SVC 33.    *
*---------------------------------------------------------------------*
TSTHHALT FUNHEAD SAVE=HTSAVE,US=NO
         L     R2,0(,R1)          R2 = ucb (arg0, the UCB address)
         LR    R0,R2              R0 = UCB common-segment address
         LA    R1,1(0)            R1 = X'00000001' (IOHALT, no OFFSET)
         SVC   33                 issue the IOHALT SVC
         FUNEXIT
*---------------------------------------------------------------------*
*  tsthio_purge(parmlist) -> INT                                     *
*     R1 = &parmlist (arg0 IS the caller's parm-list address),        *
*     SVC 16. Returns R15, the PURGE return code.                     *
*---------------------------------------------------------------------*
TSTHPURG FUNHEAD SAVE=HTSAVE,US=NO
         L     R2,0(,R1)          R2 = &parmlist (arg0)
         LR    R1,R2              R1 = &parmlist, per the PURGE SVC
         SVC   16                 issue the PURGE SVC
         FUNEXIT RC=(R15)
*
         LTORG ,
*  shared static save area (single-task probe, no concurrent callers)
         DS    0D
HTSAVE   DC    18F'0'
         END
