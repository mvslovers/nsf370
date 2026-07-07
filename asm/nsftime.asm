         TITLE 'NSFTIME - PLATFORM PRIMITIVES: MONOTONIC CLOCK + TASK ID'
*
*  NSFTIME - the two platform facts NSF asks the machine for, behind one
*            C-callable seam (Architecture Spec 7.2, and reused by NSFTMR
*            at M0-5). Two entry points:
*
*    nsf_now(NSFTIME *out)   -- store the 64-bit TOD clock into *out (STCK)
*    nsf_taskid()  -> UINT   -- current TCB address as a numeric id (PSATOLD)
*
*  The external entry names are the cc370-mangled C names (name uppercased,
*  '_' -> '@', truncated to 8, exactly as documented in asm/nsfxq.asm):
*    nsf_now    -> NSF@NOW
*    nsf_taskid -> NSF@TASK   (NSF@TASKID truncated to 8 characters)
*  They must match exactly or ld370 leaves the call adcons unresolved.
*
*  Storage model (matches include/nsftime.h):
*    NSFTIME    +0  hi              ; STCK bits 0-31 of the TOD clock
*               +4  lo              ; STCK bits 32-63
*  STCK stores a full doubleword, so a single STCK 0(out) fills both halves.
*
*  cc370 linkage (verified against libc370 and asm/nsfxq.asm): arguments
*  arrive as a fullword list addressed by R1 (arg0 at 0(R1)); results return
*  in R15. A pointer argument's value is the pointer.
*
*  RENT/REUS: register-only, no workarea and no storage service, so both
*  entries are trivially reentrant and reusable.
*
*  STATUS: like asm/nsfxq.asm, this file ASSEMBLES (as370) and LINKS (ld370)
*  into the TSTTRC test module in the normal cross build (verified at M0-4:
*  the NSF@NOW / NSF@TASK externals resolve cleanly against the cc370 call
*  sites). The host build never uses it (make test-host swaps in
*  src/nsftime_host.c via the project.toml [host].replace map). What is NOT yet
*  verified is the RUNTIME behaviour on 3.8j -- STCK actually storing a usable
*  TOD and PSATOLD being fetch-accessible in the executive's state/key -- which
*  the on-MVS suite validates at M0-6 (IFOX00 assembly + a live run).
*
*  M0-6 checklist before trusting a live run on 3.8j:
*   - External-symbol convention: VERIFIED at M0-4 -- cc370 mangles nsf_now ->
*     NSF@NOW and nsf_taskid -> NSF@TASK, and ld370 links TSTTRC clean. Re-check
*     against a libc370 .s file only if a future cc370 changes the rule.
*   - STCK operand: confirm *out is doubleword-addressable and that ignoring
*     the STCK condition code (clock set / not-set) is acceptable here.
*   - PSATOLD fetch: confirm PSA+X'21C' is fetch-accessible in the executive
*     task's state/key on 3.8j; if not, obtain the current TCB another way.
*   - Re-confirm the save-area / epilog register handling on the target.
*
         PRINT NOGEN
R1       EQU   1                       parm-list pointer on entry
R2       EQU   2                       &out (nsf_now)
R12      EQU   12                      base register
R13      EQU   13                      caller save area
R14      EQU   14                      return address
R15      EQU   15                      entry / return value
PSATOLD  EQU   X'21C'                  PSA -> current (old) TCB address
         PRINT GEN
*
*---------------------------------------------------------------------*
*  nsf_now(NSFTIME *out)  --  STCK the 64-bit TOD into *out           *
*---------------------------------------------------------------------*
NSF@NOW  CSECT
         STM   R14,R12,12(R13)         save caller regs
         BALR  R12,0
         USING *,R12
         L     R2,0(,R1)               R2 = out             (arg0)
         STCK  0(R2)                   *out = 64-bit TOD clock (8 bytes)
         LM    R14,R12,12(R13)         restore
         BR    R14
*
*---------------------------------------------------------------------*
*  nsf_taskid() -> UINT  --  R15 = current TCB address (PSATOLD)       *
*     Leaf: touches only R15 (the result), so no save area is needed.  *
*---------------------------------------------------------------------*
NSF@TASK CSECT
         L     R15,PSATOLD             R15 = current TCB address (numeric id)
         BR    R14
         END
