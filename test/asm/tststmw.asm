         TITLE 'TSTSTMW - STIMER WAIT HELPER (TIMER-ACCURACY JOB)'
*
*  TSTSTMW - a one-entry helper for the on-MVS timer-accuracy job
*            (test/mvs/tsttmacc.c):
*
*    stimer_wait(UINT centisecs)  -- STIMER WAIT for centisecs * 0.01s
*
*  This is a TASK-SYNCHRONOUS wait (STIMER TYPE=WAIT, SVC 47): no exit, no
*  ECB -- the SVC returns when the interval elapses (the form libc370 usleep
*  uses). It deliberately does NOT use the NSFTMR async STIMER-REAL + POST
*  exit seam (asm/nsfstim.asm): the accuracy job measures the STIMER TIMEBASE
*  only. The async exit-dispatch path is characterized separately at M0-6.
*
*  External symbol: the entry name is the asm() alias on stimer_wait in
*  test/mvs/tsttmacc.c (STIMWAIT); it must match this CSECT name (CLAUDE.md
*  paragraph 3, "External symbols").
*
*  as370 rules (learned the hard way -- see asm/nsfstim.asm): address the
*  static interval word by explicit displacement, never by bare-label USING;
*  keep every statement inside column 71.
*
         PRINT NOGEN
R1       EQU   1                       parm-list pointer on entry
R2       EQU   2                       centisecs
R5       EQU   5                       &interval
R12      EQU   12                      base register
R13      EQU   13                      caller save area
R14      EQU   14                      return address
         PRINT GEN
*
STIMWAIT CSECT
         STM   R14,R12,12(R13)         save caller regs
         BALR  R12,0
SWBASE   DS    0H                      R12 -> here (base for disp)
         L     R2,0(,R1)               R2 = centisecs (arg0)
         LA    R5,SWIVAL-SWBASE(,R12)  R5 = &interval
         ST    R2,0(,R5)               store the interval
         STIMER WAIT,BINTVL=(5)        block until it elapses
         LM    R14,R12,12(R13)         restore
         BR    R14
SWIVAL   DC    F'0'                    STIMER interval (0.01s units)
         END
