         TITLE 'NSFSTIM - NSFTMR STIMER SEAM: ARM / DISARM / ECB'
*
*  NSFSTIM - the real-interval-timer arming seam for NSFTMR (Spec 6.3).
*            Three C-callable entry points plus one async exit:
*
*    nsftmr_plat_arm(UINT ticks)   -- clear the ECB, arm STIMER REAL
*    nsftmr_plat_disarm()          -- TTIMER CANCEL (idempotent)
*    nsftmr_plat_ecb() -> UINT*    -- address of the timer ECB
*
*  3.8j has STIMER (SVC 47, single interval), NOT STIMERM: one STIMER
*  re-armed to the head delta gives the tick (ADR-0011). One tick = 100ms;
*  BINTVL is 0.01s units, so the interval armed is ticks*10.
*
*  ENTRY CONVENTION (issue #8): the three C-callable entries are built the
*  STANDARD cc370 way -- COPY MVSMACS + COPY PDPTOP, FUNHEAD / FUNEXIT (like
*  libc370 @@getclk.asm). A hand-rolled seam breaks the cc370 C-runtime path
*  (@@CRTGET, ABEND S0C6); see issue #8. The FUNHEAD entry name IS the 8-char
*  asm() alias in nsfstim.h -- NSFTMARM / NSFTMDIS / NSFTMECB (PR #7).
*
*  AS370 QUIRKS kept in force (each cost a live ABEND to find):
*   1) Address the static ECB / interval by EXPLICIT displacement
*      LABEL-entry(,R12) (an assembly-time constant), NEVER a bare-label
*      USING -- as370 does not resolve those reliably and drops to base 0 /
*      disp 0, which once made POST target the ECB at address 0 (ABEND S102).
*   2) as370 treats a non-blank in column 72 as a continuation flag; keep
*      every statement inside column 71.
*
*  STORAGE: this CSECT owns two static words -- the timer ECB and the
*  interval. Correct: there is one timer service (one executive task). The
*  ECB moves into an executive control block at M0-6.
*
*  STATUS: DEFERRED seam. The entry convention is fixed now (issue #8), but
*  the async STIMER-REAL + POST exit (NSFTMEXP) is invoked by the OS, not
*  called from C, so it does NOT get FUNHEAD and stays hand-rolled. Its
*  RUNTIME (a live run hit ABEND S0C6 in the exit-dispatch path) is validated
*  at M0-6, when the executive loop owns the ECB and its WAIT ECBLIST. M0-6
*  start: suspect the async exit-entry environment (save-area / addressing).
*
         COPY  MVSMACS
         COPY  PDPTOP
         CSECT ,
         PRINT GEN
*---------------------------------------------------------------------*
*  nsftmr_plat_arm(UINT ticks)  --  arm STIMER REAL, exit POSTs ECB   *
*---------------------------------------------------------------------*
NSFTMARM FUNHEAD ,                arm STIMER REAL
         L     R2,0(,R1)          R2 = ticks (arg0)
         LR    R3,R2
         SLL   R2,3               R2 = ticks*8
         SLL   R3,1               R3 = ticks*2
         AR    R2,R3              R2 = ticks*10 (0.01s units)
         LA    R5,BINTVLW-NSFTMARM(,R12)  R5 = &interval
         ST    R2,0(,R5)          store the interval
         LA    R4,TIMRECB-NSFTMARM(,R12)  R4 = &ecb
         XC    0(4,R4),0(R4)      clear the ECB
         LA    R0,NSFTMEXP-NSFTMARM(,R12) R0 = &exit
         STIMER REAL,(0),BINTVL=(5)  interval addr in R5
         FUNEXIT
*---------------------------------------------------------------------*
*  nsftmr_plat_disarm()  --  TTIMER CANCEL (register-free)            *
*---------------------------------------------------------------------*
NSFTMDIS FUNHEAD ,                cancel the pending timer
         TTIMER CANCEL
         FUNEXIT
*---------------------------------------------------------------------*
*  nsftmr_plat_ecb() -> UINT*  --  R15 = &TIMRECB                     *
*---------------------------------------------------------------------*
NSFTMECB FUNHEAD ,                return &ecb
         LA    R15,TIMRECB-NSFTMECB(,R12) R15 = &ecb
         FUNEXIT RC=(R15)
*---------------------------------------------------------------------*
*  NSFTMEXP  --  async STIMER exit (DEFERRED, OS-invoked, NOT FUNHEAD)*
*     Entered with R13 -> save area; self-bases with BALR; ECB by     *
*     explicit disp. Runtime validated at M0-6 (S0C6 note above).     *
*---------------------------------------------------------------------*
NSFTMEXP SAVE  (14,12)            save regs in the supplied area
         BALR  R12,0
EXPBASE  DS    0H                 R12 -> here (base for disp)
         LA    R1,TIMRECB-EXPBASE(,R12)   R1 = &ecb
         POST  (1)                POST the ECB (code 0)
         RETURN (14,12)           restore, return to MVS
*
*  Static timer state (one timer service; see STORAGE note).
BINTVLW  DC    F'0'               STIMER interval (0.01s units)
TIMRECB  DC    F'0'               the timer ECB
         END
