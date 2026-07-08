         TITLE 'NSFXQ - EXIT->MAINLINE HANDOFF (CS LIFO STACK)'
*
*  NSFXQ - the lock-free single-consumer LIFO stack an asynchronous MVS
*          exit uses to hand a pre-allocated QELEM to the executive task
*          (Architecture Spec 4.2). Three C-callable entry points:
*
*    xq_init(XQ *xq)              -- head = 0
*    xq_push(XQ *xq, QELEM *e)    -- exit side: prepend e (CS loop)
*    xq_drain(XQ *xq) -> QELEM*   -- executive: swap out the chain
*
*  ENTRY CONVENTION (issue #8): built the STANDARD cc370 way -- COPY MVSMACS
*  + COPY PDPTOP, FUNHEAD / FUNEXIT, modeled on libc370 @@getclk.asm. A
*  hand-rolled STM/BALR/USING seam breaks the cc370 C-runtime path (@@CRTGET,
*  ABEND S0C6); see issue #8. The FUNHEAD entry name IS the 8-char asm() alias
*  in include/nsfxq.h (NSFXINIT / NSFXPUSH / NSFXDRAN), character for character
*  (PR #7).
*
*  RS-FORMAT OPERANDS (#5): the CS / LM operands are written D(B), NOT D(,B).
*  as370 mis-assembles the empty-index RX form D(,B) on an RS instruction with
*  base register 0 (cc370 issue #12). The RX-format L operands use D(,B).
*
*  ABA safety: exactly one consumer, which removes work by swapping out the
*  WHOLE chain at once (xq_drain); individual elements are never popped, so a
*  node cannot be freed and re-pushed to reappear as a stale-but-equal head.
*  Do NOT add a per-element pop.
*
*  cc370 linkage: args are a fullword list at R1 (arg0 at 0(R1), arg1 at
*  4(R1)); a pointer result returns in R15 via FUNEXIT RC=(Rn).
*
*  STATUS: assembles and links; the CS loop's RUNTIME on 3.8j is validated by
*  the on-MVS TSTXQ suite at M0-6 (the host build swaps src/nsfxq_host.c). The
*  entry convention is fixed now (issue #8); only the runtime stays deferred.
*
         COPY  MVSMACS
         COPY  PDPTOP
         CSECT ,
         PRINT GEN
*---------------------------------------------------------------------*
*  xq_init(XQ *xq)  --  head = 0                                       *
*---------------------------------------------------------------------*
NSFXINIT FUNHEAD ,                head = 0
         L     R2,0(,R1)          R2 = xq (arg0)
         SLR   R3,R3              R3 = 0
         ST    R3,0(,R2)          xq->head = 0
         FUNEXIT
*---------------------------------------------------------------------*
*  xq_push(XQ *xq, QELEM *e)                                           *
*     loop: R4 = head ; e->next = head ; CS(head==R4 -> head=e)        *
*---------------------------------------------------------------------*
NSFXPUSH FUNHEAD ,                prepend e (CS retry loop)
         L     R2,0(,R1)          R2 = xq (arg0)
         L     R3,4(,R1)          R3 = e (arg1, swap-in value)
XQPLOOP  L     R4,0(,R2)          R4 = current head
         ST    R4,0(,R3)          e->next = current head
         CS    R4,R3,0(R2)        head==R4 ? head=e : R4=head,retry
         BNE   XQPLOOP            raced: retry with the reloaded head
         FUNEXIT
*---------------------------------------------------------------------*
*  xq_drain(XQ *xq) -> QELEM*                                          *
*     loop: R4 = head ; CS(head==R4 -> head=0) ; return old head       *
*---------------------------------------------------------------------*
NSFXDRAN FUNHEAD ,                swap out the whole chain
         L     R2,0(,R1)          R2 = xq (arg0)
         SLR   R3,R3              R3 = 0 (new empty head)
XQDLOOP  L     R4,0(,R2)          R4 = current head
         CS    R4,R3,0(R2)        head==R4 ? head=0 : R4=head,retry
         BNE   XQDLOOP            raced: retry
         FUNEXIT RC=(R4)          return the drained chain (old head)
         END
