         TITLE 'NSFXQ - INTERRUPT-SAFE EXIT->MAINLINE HANDOFF (CS LIFO STACK)'
*
*  NSFXQ - the lock-free single-consumer LIFO stack an asynchronous MVS
*          exit uses to hand a pre-allocated QELEM to the executive task
*          (Architecture Spec 4.2). Three C-callable entry points:
*
*    xq_init(XQ *xq)                     -- head = 0
*    xq_push(XQ *xq, QELEM *e)           -- exit side: prepend e (CS loop)
*    xq_drain(XQ *xq)  -> QELEM*         -- executive: swap out the chain
*
*  The external entry names are the explicit asm() aliases pinned on the
*  xq_* declarations in include/nsfxq.h: xq_init/xq_push/xq_drain resolve to
*  NSFXINIT / NSFXPUSH / NSFXDRAN. Each CSECT label below MUST match its alias
*  character-for-character, or ld370 leaves the call adcons unresolved. Pinning
*  the names this way makes the boundary independent of cc370's 8-char
*  '_' -> '@' name mangling (see CLAUDE.md §3, "External symbols").
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
*  RS-FORMAT OPERANDS: the LM/STM/CS operands below are written D(B) --
*  NOT D(,B). as370 silently assembles the empty-index RX form D(,B) on an
*  RS instruction with base register 0, turning the access into an absolute
*  low-storage reference (CS on 0(0), LM restore from PSA 0x14) -- see
*  cc370 issue #12. The RX-format L/ST operands legitimately use D(,B).
*
*  RENT/REUS: register-only, no workarea and no storage service, so all
*  three entries are trivially reentrant and reusable.
*
*  STATUS: this file assembles (as370) and links (ld370) into the TSTXQ test
*  module in the normal cross build; the host build never uses it (make
*  test-host swaps in src/nsfxq_host.c via the project.toml [host].replace
*  map). Its RUNTIME behaviour on the target is validated by the on-MVS
*  suite, which is deferred to M0-6 (roadmap M0-6) -- so the CS logic below
*  is exercised for real by the host tests through the atomic shim, not yet
*  on 3.8j.
*
*  M0-6 checklist before trusting a live run on 3.8j:
*   - External-symbol convention: the C side pins each entry with an explicit
*     asm() alias in include/nsfxq.h (xq_push -> =V(NSFXPUSH)); the CSECT names
*     below match those aliases exactly, so resolution no longer depends on
*     cc370's '_' -> '@' truncation rule -- the rule that silently collided the
*     buf_* pairs before this convention (see CLAUDE.md §3). Confirm the TSTXQ
*     link stays clean under the on-MVS build.
*   - Re-confirm the save-area / epilog register handling on the target: the
*     STM/LM offsets and the R15-preserving xq_drain epilog, none of which the
*     host shim exercises.
*   - Run TSTXQ under make test-mvs to validate the CS loop on the real system.
*
         PRINT NOGEN
R0       EQU   0
R1       EQU   1                       parm-list pointer on entry
R2       EQU   2                       &xq->head
R3       EQU   3                       new element (push) / swap-in value
R4       EQU   4                       current head (CS compare operand)
R12      EQU   12                      base register
R13      EQU   13                      caller save area
R14      EQU   14                      return address
R15      EQU   15                      entry / return value
         PRINT GEN
*
*---------------------------------------------------------------------*
*  xq_init(XQ *xq)  --  R2 = &xq->head ; head = 0                      *
*---------------------------------------------------------------------*
NSFXINIT CSECT
         STM   R14,R12,12(R13)         save caller regs
         BALR  R12,0
         USING *,R12
         L     R2,0(,R1)               R2 = xq              (arg0)
         SLR   R3,R3                   R3 = 0
         ST    R3,0(,R2)               xq->head = 0
         LM    R14,R12,12(R13)         restore
         BR    R14
*
*---------------------------------------------------------------------*
*  xq_push(XQ *xq, QELEM *e)                                           *
*     loop: R4 = head ; e->next = head ; CS(head==R4 -> head=e)        *
*---------------------------------------------------------------------*
NSFXPUSH CSECT
         STM   R14,R12,12(R13)         save caller regs
         BALR  R12,0
         USING *,R12
         L     R2,0(,R1)               R2 = xq              (arg0)
         L     R3,4(,R1)               R3 = e (also swap-in value)
XQPLOOP  L     R4,0(,R2)               R4 = current head
         ST    R4,0(,R3)               e->next = current head
         CS    R4,R3,0(R2)             head==R4 ? head=e,CC0 : R4=head,CC1
         BNE   XQPLOOP                 raced: retry with the reloaded head
         LM    R14,R12,12(R13)         restore
         BR    R14
*
*---------------------------------------------------------------------*
*  xq_drain(XQ *xq) -> QELEM*                                          *
*     loop: R4 = head ; CS(head==R4 -> head=0) ; return old head       *
*---------------------------------------------------------------------*
NSFXDRAN CSECT
         STM   R14,R12,12(R13)         save caller regs
         BALR  R12,0
         USING *,R12
         L     R2,0(,R1)               R2 = xq              (arg0)
         SLR   R3,R3                   R3 = 0 (new empty head)
XQDLOOP  L     R4,0(,R2)               R4 = current head
         CS    R4,R3,0(R2)             head==R4 ? head=0,CC0 : R4=head,CC1
         BNE   XQDLOOP                 raced: retry
         LR    R15,R4                  return the drained chain (old head)
         L     R14,12(,R13)            restore R14 (R15 holds the result)
         LM    R0,R12,20(R13)          restore R0-R12 (leave R15 = result)
         BR    R14
         END
