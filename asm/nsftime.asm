         TITLE 'NSFTIME - PLATFORM PRIMITIVES: CLOCK + TASK ID'
*
*  NSFTIME - the two platform facts NSF asks the machine for, behind one
*            C-callable seam (Architecture Spec 7.2, reused by NSFTMR):
*
*    nsf_now(NSFTIME *out)   -- store the 64-bit TOD clock (STCK)
*    nsf_taskid()  -> UINT   -- current TCB address (PSATOLD)
*
*  ENTRY CONVENTION (issue #8): every C-callable routine is built the
*  STANDARD cc370 way -- COPY MVSMACS + COPY PDPTOP, FUNHEAD prologue,
*  FUNEXIT epilogue -- modeled on libc370/asm/@@getclk.asm (NSFNOW is
*  that routine). A hand-rolled STM/BALR/USING seam does NOT set up the
*  ENTRY / name eyecatcher / R12=R15 base the cc370 C-runtime path
*  (@@CRTGET) relies on, and breaks the next C call (ABEND S0C6). Proven
*  by the issue #8 staged isolation; never hand-roll a C-callable routine.
*
*  EXTERNAL SYMBOLS: the FUNHEAD entry name IS the 8-char asm() alias in
*  include/nsftime.h -- NSFNOW / NSFTASK -- character for character, so the
*  C =V(...) still resolves (PR #7; CLAUDE.md paragraph 3).
*
*  Linkage: args are a fullword list at R1 (arg0 at 0(R1)); a value result
*  returns in R15 via FUNEXIT RC=(Rn).
*
*  STATUS: nsf_now is RUNTIME-VALIDATED on 3.8j (issue #8 stage 2 = CC 0).
*  STCK stores a doubleword, so one STCK 0(out) fills both NSFTIME halves.
*
         COPY  MVSMACS
         COPY  PDPTOP
         CSECT ,
         PRINT GEN
PSATOLD  EQU   X'21C'             PSA -> current TCB
*---------------------------------------------------------------------*
*  nsf_now(NSFTIME *out)  --  STCK the 64-bit TOD into *out           *
*---------------------------------------------------------------------*
NSFNOW   FUNHEAD ,                store the TOD clock
         L     R2,0(,R1)          R2 = out (arg0)
         STCK  0(R2)              *out = 64-bit TOD (8 bytes)
         FUNEXIT
*---------------------------------------------------------------------*
*  nsf_taskid() -> UINT  --  R15 = current TCB address (PSATOLD).     *
*     PSATOLD is an absolute PSA offset, so L uses base 0 by design.  *
*---------------------------------------------------------------------*
NSFTASK  FUNHEAD ,                current task id
         L     R15,PSATOLD        R15 = current TCB addr
         FUNEXIT RC=(R15)
         END
