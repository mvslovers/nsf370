         TITLE 'NSFXQ - INTERRUPT-SAFE EXIT->MAINLINE HANDOFF (CS LIFO STACK)'
*
*  NSFXQ - the lock-free single-consumer LIFO stack an asynchronous MVS
*          exit uses to hand a pre-allocated QELEM to the executive task
*          (Architecture Spec 4.2). Two entry points:
*
*    XQPUSH  xq_push(XQ *xq, QELEM *e)   -- exit side: prepend e (CS loop)
*    XQDRAIN xq_drain(XQ *xq)            -- executive: swap out the chain
*
*  Storage model (matches include/nsfxq.h / include/nsfque.h):
*    XQ         +0  head            ; QELEM* top of stack (0 = empty)
*    QELEM      +0  next            ; link (this stack uses next only)
*               +4  prev            ; unused here
*
*  cc370 linkage (verified against libc370 getmain.s/malloc.s): arguments
*  arrive as a fullword list addressed by R1 -- arg0 at 0(R1), arg1 at
*  4(R1); results return in R15. A pointer argument's value is the pointer.
*
*  RENT/REUS: no workarea, no storage service -- register-only, so both
*  entries are trivially reentrant and reusable.
*
*  STATUS: SKELETON. The host build never assembles this file -- make
*  test-host uses src/nsfxq_host.c via the project.toml [host].replace map.
*  On-MVS assembly and validation are deferred to M0-6 (roadmap M0-6); the
*  algorithm and linkage below are the starting point for that work and have
*  not yet been assembled with IFOX00 or run on the target.
*
         PRINT NOGEN
R0       EQU   0
R1       EQU   1
R2       EQU   2                       parm-list / xq head address
R3       EQU   3                       new element (push)
R4       EQU   4                       current head (compare operand)
R5       EQU   5                       swap-in value
R12      EQU   12                      base register
R13      EQU   13
R14      EQU   14                      return address
R15      EQU   15                      entry / return value
         PRINT GEN
NEXT     EQU   0                       offset of QELEM.next
HEAD     EQU   0                       offset of XQ.head
*
*---------------------------------------------------------------------*
*  XQPUSH  --  xq_push(XQ *xq, QELEM *e)                               *
*     R2 = &xq->head , R3 = e                                          *
*     loop: R4 = head ; e->next = head ; CS(head==R4 -> head=e)        *
*---------------------------------------------------------------------*
XQPUSH   CSECT
         STM   R14,R12,12(R13)         save caller regs
         BALR  R12,0
         USING *,R12
         L     R2,0(,R1)               R2 = xq            (arg0)
         L     R3,4(,R1)               R3 = e             (arg1)
XQPLOOP  DS    0H
         L     R4,HEAD(,R2)            R4 = current head
         ST    R4,NEXT(,R3)            e->next = current head
         LR    R5,R3                   R5 = swap-in value (e)
         CS    R4,R5,HEAD(,R2)         head==R4 ? head=e,CC0 : R4=head,CC1
         BNE   XQPLOOP                 raced: retry with the reloaded head
         LM    R14,R12,12(R13)         restore
         BR    R14
*
*---------------------------------------------------------------------*
*  XQDRAIN --  xq_drain(XQ *xq)                                        *
*     R2 = &xq->head                                                   *
*     loop: R4 = head ; CS(head==R4 -> head=0) ; return old head       *
*---------------------------------------------------------------------*
XQDRAIN  CSECT
         STM   R14,R12,12(R13)         save caller regs
         BALR  R12,0
         USING *,R12
         L     R2,0(,R1)               R2 = xq            (arg0)
         SLR   R5,R5                   R5 = 0 (new empty head)
XQDLOOP  DS    0H
         L     R4,HEAD(,R2)            R4 = current head
         CS    R4,R5,HEAD(,R2)         head==R4 ? head=0,CC0 : R4=head,CC1
         BNE   XQDLOOP                 raced: retry
         LR    R15,R4                  return the drained chain (old head)
         L     R14,12(,R13)            restore R14 (R15 holds the result)
         LM    R0,R12,20(R13)          restore R0-R12 (leave R15 = result)
         BR    R14
         END
