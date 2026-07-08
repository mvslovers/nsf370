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
*  STATUS: VALIDATED on 3.8j at M0-6. The three C-callable entries are FUNHEAD
*  (issue #8); the async STIMER-REAL + POST exit (NSFTMEXP) is OS-invoked, so it
*  does NOT get FUNHEAD and is built to the documented MVS 3.8 STIMER-exit
*  linkage (GC28-0683; see ADR-0017 for the 7-step contract). test/mvs/tstevtm.c
*  ran the loop over 10 heartbeats at ~100 ms and shut down clean (CC 0) -- the
*  earlier ABEND S0C6 (a hand-rolled entry-convention shortcut) is gone.
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
*  NSFTMEXP  --  async STIMER REAL exit (OS-invoked, NOT FUNHEAD).    *
*                Built to the documented MVS 3.8 STIMER-exit linkage  *
*                (GC28-0683); validated on MVS at M0-6 (issue #8 /    *
*                the timer-wakeup ADR). Entered with R15 = exit entry *
*                address, R14 = return, R13 = a provided save area.   *
*                It POSTs the timer ECB and RE-ARMS STIMER (one-shot),*
*                giving a periodic ~100 ms heartbeat.                 *
*---------------------------------------------------------------------*
NSFTMEXP DS    0H
         STM   R14,R12,12(R13)    save into the provided SA
         LR    R12,R15            base via R12=R15 (macros used)
         USING NSFTMEXP,R12
*  chain our OWN save area (we issue POST + STIMER SVCs)
         LA    R1,EXPSAVE-NSFTMEXP(,R12)  R1 = our SA
         ST    R13,4(,R1)         our SA back-ptr = caller SA
         ST    R1,8(,R13)         caller SA fwd-ptr = our SA
         LR    R13,R1             R13 = our save area
*  POST the timer ECB (explicit disp; S102-safe, never base 0)
         LA    R1,TIMRECB-NSFTMEXP(,R12)  R1 = &ecb
         SLR   R0,R0              post code 0
         POST  (1),(0)
*  re-arm STIMER REAL for the next interval (one-shot)
         LR    R0,R12             R0 = &NSFTMEXP (exit address)
         LA    R1,BINTVLW-NSFTMEXP(,R12)  R1 = &interval
         STIMER REAL,(0),BINTVL=(1)
*  unchain, restore from the provided SA, return
         L     R13,4(,R13)        R13 = caller save area
         LM    R14,R12,12(R13)    restore caller regs
         BR    R14
*
*  Static timer state (one timer service; see STORAGE note). Placed right
*  after the exit code so the exit reaches them R15/R12-relative.
BINTVLW  DC    F'0'               STIMER interval (0.01s units)
TIMRECB  DC    F'0'               the timer ECB
EXPSAVE  DC    18F'0'             the exit's own 72-byte save area
         END
