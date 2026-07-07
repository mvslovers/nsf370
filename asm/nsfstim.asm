         TITLE 'NSFSTIM - NSFTMR STIMER SEAM: ARM / DISARM / ECB'
*
*  NSFSTIM - the real-interval-timer arming seam for NSFTMR (Architecture
*            Spec 6.3). Three C-callable entry points plus one async exit:
*
*    nsftmr_plat_arm(UINT ticks)   -- clear the ECB, arm STIMER REAL for
*                                     ticks*100ms; the exit POSTs the ECB
*    nsftmr_plat_disarm()          -- TTIMER CANCEL (idempotent)
*    nsftmr_plat_ecb() -> UINT*    -- address of the timer ECB (WAIT target)
*
*  3.8j provides STIMER (SVC 47, single interval per task), NOT STIMERM: one
*  STIMER re-armed to the head delta by nsftmr_run gives the intended tick
*  (include/nsftmr.h, docs/adr/ADR-0011). One tick = 100ms and STIMER BINTVL
*  is 0.01s units, so the interval armed is BINTVL = ticks*10.
*
*  EXTERNAL SYMBOLS: the entry names are the asm() aliases in nsfstim.h --
*  NSFTMARM / NSFTMDIS / NSFTMECB, each an ENTRY of the one NSFSTIM CSECT so
*  they share the static ECB. Each ENTRY name must match its alias exactly
*  (CLAUDE.md paragraph 3, "External symbols").
*
*  cc370 linkage: args are a fullword list at R1 (arg0 at 0(R1)); results
*  return in R15. A pointer argument's value is the pointer.
*
*  AS370 QUIRKS (both cost a live S102 ABEND to find):
*   1) as370 does not reliably resolve a BARE-LABEL data reference to a base
*      register via USING across multiple entry points -- it silently falls
*      back to base 0 / disp 0 and hits low storage (this once made the exit
*      POST the ECB at address 0 -> ABEND S102). So this module NEVER uses
*      USING for data: every entry self-bases with BALR R12,0 and addresses
*      the static ECB / interval with an explicit displacement
*      `LABEL-base(,R12)` (an assembly-time constant) or via a register.
*   2) as370 treats a non-blank in column 72 as a continuation flag and
*      silently merges the next source line. Every statement here is kept
*      well inside column 71.
*
*  STORAGE: the CSECT owns two static words -- the timer ECB and the binary
*  interval. Correct: there is one timer service (one executive task), so a
*  single module-resident ECB is the object the exit POSTs and the loop WAITs
*  on. The ECB moves into an executive control block at M0-6.
*
*  STATUS: assembles (as370) and cross-links in the normal build. Like
*  asm/nsfxq.asm and asm/nsftime.asm this is a DEFERRED seam: its RUNTIME on
*  3.8j is not yet validated. A live run fixed one real bug (S102 -- data
*  addressed at base 0; see the AS370 QUIRKS note) but then hit ABEND S0C6
*  (specification exception) in the async STIMER-REAL exit-dispatch path. That
*  path is validated at M0-6, when the executive event loop owns the timer ECB
*  and its WAIT ECBLIST; the timebase ACCURACY is measured separately now by
*  test/mvs/tsttmacc.c (STIMER WAIT). M0-6 S0C6 starting point: suspect the
*  async exit-entry environment -- base / save-area setup or branch alignment
*  at exit entry -- compared against libc370 @@ecbtwl.c EXITDRVR.
*
         PRINT NOGEN
R0       EQU   0
R1       EQU   1                       parm-list pointer on entry
R2       EQU   2                       working (ticks)
R3       EQU   3                       working (ticks*2)
R4       EQU   4                       &ecb
R5       EQU   5                       &interval
R12      EQU   12                      base register
R13      EQU   13                      caller save area
R14      EQU   14                      return address
R15      EQU   15                      entry / return value
         PRINT GEN
*
NSFSTIM  CSECT
         ENTRY NSFTMARM
         ENTRY NSFTMDIS
         ENTRY NSFTMECB
*---------------------------------------------------------------------*
*  nsftmr_plat_arm(UINT ticks)  --  arm STIMER REAL, exit POSTs ECB   *
*---------------------------------------------------------------------*
NSFTMARM STM   R14,R12,12(R13)         save caller regs
         BALR  R12,0
ARMBASE  DS    0H                      R12 -> here (base for disps)
         L     R2,0(,R1)               R2 = ticks (arg0)
         LR    R3,R2
         SLL   R2,3                    R2 = ticks*8
         SLL   R3,1                    R3 = ticks*2
         AR    R2,R3                   R2 = ticks*10 (0.01s units)
         LA    R5,BINTVLW-ARMBASE(,R12)   R5 = &interval
         ST    R2,0(,R5)               store the interval
         LA    R4,TIMRECB-ARMBASE(,R12)   R4 = &ecb
         XC    0(4,R4),0(R4)           clear the ECB
         LA    R0,NSFTMEXP-ARMBASE(,R12)  R0 = &exit
         STIMER REAL,(0),BINTVL=(5)    interval addr in R5
         LM    R14,R12,12(R13)         restore
         BR    R14
*---------------------------------------------------------------------*
*  nsftmr_plat_disarm()  --  TTIMER CANCEL (register-free, no base)   *
*---------------------------------------------------------------------*
NSFTMDIS STM   R14,R12,12(R13)         save caller regs
         TTIMER CANCEL
         LM    R14,R12,12(R13)         restore
         BR    R14
*---------------------------------------------------------------------*
*  nsftmr_plat_ecb() -> UINT*  --  R15 = &TIMRECB                     *
*---------------------------------------------------------------------*
NSFTMECB STM   R14,R12,12(R13)         save caller regs
         BALR  R12,0
ECBBASE  DS    0H                      R12 -> here (base for disp)
         LA    R15,TIMRECB-ECBBASE(,R12)  R15 = &ecb
         L     R14,12(,R13)            restore R14 (R15 = result)
         LM    R0,R12,20(R13)          restore R0-R12, keep R15
         BR    R14
*---------------------------------------------------------------------*
*  NSFTMEXP  --  async STIMER exit: POST the ECB, return              *
*     Entered with R13 -> save area (SAVE/RETURN like libc370         *
*     @@ecbtwl.c). Self-bases with BALR; ECB by explicit disp.        *
*---------------------------------------------------------------------*
NSFTMEXP SAVE  (14,12)                 save regs in the supplied area
         BALR  R12,0
EXPBASE  DS    0H                      R12 -> here (base for disp)
         LA    R1,TIMRECB-EXPBASE(,R12)   R1 = &ecb
         POST  (1)                      POST the ECB (code 0)
         RETURN (14,12)                restore, return to MVS
*
*  Static timer state (one timer service; see STORAGE note).
BINTVLW  DC    F'0'                    STIMER interval (0.01s units)
TIMRECB  DC    F'0'                    the timer ECB
         END
