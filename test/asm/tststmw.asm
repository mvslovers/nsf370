         TITLE 'TSTSTMW - STIMER WAIT HELPER (TIMER-ACCURACY JOB)'
*
*  TSTSTMW - a one-entry helper for the on-MVS timer-accuracy job
*            (test/mvs/tsttmacc.c):
*
*    stimer_wait(UINT centisecs)  -- STIMER WAIT for centisecs * 0.01s
*
*  A TASK-SYNCHRONOUS wait (STIMER TYPE=WAIT, SVC 47): no exit, no ECB (the
*  form libc370 usleep uses). It measures the STIMER TIMEBASE only; the async
*  exit-dispatch path is characterized at M0-6.
*
*  ENTRY CONVENTION (issue #8): built the STANDARD cc370 way -- COPY MVSMACS +
*  COPY PDPTOP, FUNHEAD / FUNEXIT (like libc370 @@getclk.asm). The FUNHEAD
*  entry name IS the 8-char asm() alias on stimer_wait in tsttmacc.c (STIMWAIT).
*
*  as370: address the static interval by explicit displacement, never a
*  bare-label USING; keep every statement inside column 71.
*
         COPY  MVSMACS
         COPY  PDPTOP
         CSECT ,
         PRINT GEN
*---------------------------------------------------------------------*
*  stimer_wait(UINT centisecs)  --  STIMER WAIT for centisecs*0.01s   *
*---------------------------------------------------------------------*
STIMWAIT FUNHEAD ,                block for the interval
         L     R2,0(,R1)          R2 = centisecs (arg0)
         LA    R5,SWIVAL-STIMWAIT(,R12)   R5 = &interval
         ST    R2,0(,R5)          store the interval
         STIMER WAIT,BINTVL=(5)   block until it elapses
         FUNEXIT
SWIVAL   DC    F'0'               STIMER interval (0.01s units)
         END
