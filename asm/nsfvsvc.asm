*----------------------------------------------------------------------
* nsfvsvc.asm -- M5 Stage-0a' SVC cross-AS probe: the private SVC routine.
*
* ADR-0038 (supersedes ADR-0036's SSI transport).  This is the routine a
* dynamically installed private SVC dispatches, in the CALLER's address
* space, to hand a request across to the probe STC (NSFV) and back.  It is
* the SVC analogue of Stage-0a's SSI router (retired; ADR-0036) -- same anchor,
* same __xmpost cross-AS wake, same in-flight discipline -- but reached by
* SVC dispatch instead of IEFSSREQ, so it serves an UNAUTHORIZED problem-
* state client (no APF): the SVC is the APF-free unauthorized->authorized
* transition (ADR-0038).  No NSFRQE, no socket, no protocol: it round-trips
* a 32-bit token, staged on an empty payload before M5-2 rides the real
* request over this transport.
*
* Written in assembler (Mike's call), NOT cc370 C: an SVC routine has no C
* runtime (the cc370 prologue's @@CRTGET wants a per-TCB CRT an arbitrary
* caller's TCB may not have usable), and the register-in/out convention is
* native to assembler.  Modelled on the CBT/mvs38j-ip stolen-slot transport
* SVC (STCPSVC) and Type-3 SVC shape (igc0024e.asm), plus the libc370 branch
* POST (@@xmpost.c).
*
* NAMING (Mike's question).  A STOLEN-slot SVC routine takes an ARBITRARY
* CSECT name, NOT the IGCnnn scheme: the ancestor's transport SVC is CSECT
* "STCPSVC" (cbt571/PDS/STCPSVC), while its SYSGEN-installed auth SVC is
* "IGC0024E" (SVC 245).  The IGCnnn scheme is only for SVCs MVS loads BY NAME
* (SYSGEN / the standard SVC loader).  We steal the SVCTABLE slot and install
* a RESIDENT CSA entry point directly (STCPSVC0), so the loader is bypassed and
* the name is free -- NSFVSVC follows STCPSVC's precedent.
*
* RENT: entered concurrently from many address spaces/tasks -- no writable
* statics (the NSFVANCH word is patched ONCE by the STC before the slot is
* stolen, then read-only; the probe's single-client-sequential model makes the
* shared anchor scratch safe -- ADR-0038).  project.toml: entry=NSFVSVC,
* startup=false, ac=1; __loadhi'd into CSA by the STC.
*
* Entry (Type-3 SVC, set by the SVC FLIH -- STCPSVC / igc0024e.asm):
*   supervisor state, PSW key 0, ENABLED
*   R1 = issuer R1 = A(NSFV_REQ) in the CALLER's storage (the M5-2 shape)
*   R5 = A(SVRB)      R6 = A(entry point = our load point in CSA)
*   R7 = A(caller ASCB)   R13 = issuer R13 (18-word savearea)   R14 = return
* Exit:  BR R14.  Per STCPSVC: "R0, R1, R15 are the only regs returned to the
*   issuer; R2-R14 are restored by the system."  We set R15 = rc and also write
*   the full result (echo/seq/rc) into the caller's NSFV_REQ block, which is
*   therefore authoritative and independent of the register-return path.
*
* Column-71 discipline (CLAUDE.md 3): instruction-line comments stay short;
* rationale lives in these full-width '*' blocks.  CS/LM operands are D(B).
*----------------------------------------------------------------------
NSFVSVC  CSECT
R0       EQU   0
R1       EQU   1
R2       EQU   2
R3       EQU   3
R4       EQU   4
R5       EQU   5
R6       EQU   6
R7       EQU   7
R8       EQU   8
R9       EQU   9
R10      EQU   10
R11      EQU   11
R12      EQU   12
R13      EQU   13
R14      EQU   14
R15      EQU   15
*----------------------------------------------------------------------
*  NSFV_ANCHOR field offsets -- MIRROR of include/nsfvsvc.h (guarded there
*  by NSF_SIZE_ASSERT at cross-compile).
*----------------------------------------------------------------------
ANCEYE   EQU   0                  CL8  "NSFVANCR"
ANCFLAG  EQU   12                 F    ACTIVE = X'80000000'
ANCSECB  EQU   16                 F    server_ecb  (STC WAIT target)
ANCSASCB EQU   20                 A    server_ascb (POST target)
ANCINFL  EQU   24                 F    inflight
ANCSTATE EQU   28                 F    req_state
ANCTOKEN EQU   32                 F    req_token
ANCRECB  EQU   36                 F    reply_ecb   (our WAIT target)
ANCRASCB EQU   40                 A    req_ascb    (caller ASCB)
ANCSERVD EQU   44                 F    served
ANCSAVE  EQU   48                 18F  POST register save area
ANCXFUNC EQU   120                F    transform (ECHO/XFER)
ANCXLEN  EQU   124                F    bytes staged this chunk
ANCSTAGE EQU   128                CSA staging buffer (2048)
*  NSFV_REQ field offsets (caller's block, R8 = A(req))
REQEYE   EQU   0                  CL4  "NSFV"
REQFUNC  EQU   4                  F    request function
REQTOKN  EQU   8                  F    in token / out echo (+1)
REQRC    EQU   12                 F    out rc
REQSEQ   EQU   16                 F    out served snapshot
REQUBUF  EQU   20                 A    XFER caller ubuf addr
REQULEN  EQU   24                 F    XFER bytes to move
*  state + rc constants (mirror nsfvsvc.h)
STFREE   EQU   0
STPEND   EQU   1
STDONE   EQU   2
RCOK     EQU   0
RCINVAL  EQU   4
RCCORR   EQU   8
RCNOREQ  EQU   12
*  request functions + MVCK copy constants (mirror nsfvsvc.h)
FNECHO   EQU   1
FNXFER   EQU   2
XFCHUNK  EQU   2048               max ulen moved per SVC call
MVCKMAX  EQU   255                bytes per MVCK piece
MVCKK8   EQU   X'80'              MVCK source key 8
*----------------------------------------------------------------------
         USING NSFVSVC,R6         base = our entry (R6, FLIH-set)
         B     NSFVGO             skip the STC-patched anchor word
NSFVANCH DC    A(0)               +4: anchor addr, patched by STC
*----------------------------------------------------------------------
NSFVGO   DS    0H
         LR    R8,R1              R8 = A(NSFV_REQ) (caller block)
         LTR   R8,R8              null request pointer?
         BZ    BADREQ             yes -> INVALID (rc in R15 only)
         CLC   REQEYE(4,R8),=CL4'NSFV'   our caller?
         BNE   BADREQ             no  -> INVALID (rc in R15 only)
*----------------------------------------------------------------------
*  Locate + validate the CSA anchor (published in NSFVANCH by the STC).
*----------------------------------------------------------------------
         L     R2,NSFVANCH        R2 = A(anchor)
         LTR   R2,R2
         BZ    BADANC             not published -> CORRUPT
         CLC   ANCEYE(8,R2),=CL8'NSFVANCR'
         BNE   BADANC
         TM    ANCFLAG(R2),X'80'  ACTIVE?  (X'80000000' high byte)
         BNO   BADANC             quiescing -> CORRUPT
*----------------------------------------------------------------------
*  Take the one request slot (single-client sequential probe: reject a
*  concurrent client rather than corrupt the slot).  Reject BEFORE the
*  in-flight increment, so a rejected caller leaves inflight untouched.
*----------------------------------------------------------------------
         L     R3,ANCSTATE(,R2)
         LTR   R3,R3              FREE (== 0)?
         BNZ   SLOTBSY            no -> NOREQ
*----------------------------------------------------------------------
*  Mark in-flight (shutdown clears ACTIVE and drains this to zero before it
*  frees the CSA).  CS loop = the in-flight __uinc of Stage-0a's SSI router.
*----------------------------------------------------------------------
         L     R3,ANCINFL(,R2)
UINCLP   LR    R4,R3
         LA    R4,1(,R4)
         CS    R3,R4,ANCINFL(R2)
         BNE   UINCLP
*----------------------------------------------------------------------
*  Dispatch on req.func.  ECHO stages the token; XFER first copies the
*  caller's ubuf into the CSA staging buffer (write-in), then stages xlen.
*  Both publish req_state = PENDING LAST (after every other field), so an
*  STC wake never services a half-formed slot; and zero the reply ECB first
*  so a stale post cannot false-wake the WAIT.
*----------------------------------------------------------------------
         L     R3,REQFUNC(,R8)             request function
         C     R3,=A(FNXFER)               XFER?
         BE    XFERIN                      yes -> write-in + xlen
*  ECHO: stage the token.  Set xfunc = ECHO so an ECHO after an XFER is not
*  misdispatched by the STC (which switches on the staged xfunc).
         XC    ANCRECB(4,R2),ANCRECB(R2)   reply_ecb = 0
         L     R3,REQTOKN(,R8)             read caller token
         ST    R3,ANCTOKEN(,R2)            stage token
         LA    R3,FNECHO
         ST    R3,ANCXFUNC(,R2)            xfunc = ECHO
         ST    R7,ANCRASCB(,R2)            caller ASCB (POST target)
         LA    R3,STPEND
         ST    R3,ANCSTATE(,R2)            publish PENDING
         B     DOPOST
*----------------------------------------------------------------------
*  XFER write-in.  Clamp L = min(ulen, XFCHUNK): a > chunk ulen would run
*  MVCK off the end of stage[] (the last anchor field) into adjacent CSA --
*  an IPL-class overrun.  Stage xlen = L (in CSA, survives POST/WAIT, reused
*  for read-out).  Then MVCK the caller's ubuf (source key 8) into stage
*  (dst key 0), <= 255 bytes/piece -- raw D9, since as370 mis-assembles the
*  MVCK mnemonic (drops the R1/R3 registers); see tstmvck.c.  The piece
*  length is saved in R0 (MVCK is not trusted to preserve R1) and used to
*  advance, so the loop never depends on a register MVCK may clobber.
*----------------------------------------------------------------------
XFERIN   DS    0H
         L     R0,REQULEN(,R8)             R0 = ulen
         C     R0,=A(XFCHUNK)              > staging size?
         BNH   XFISTX
         L     R0,=A(XFCHUNK)              clamp to staging size
XFISTX   ST    R0,ANCXLEN(,R2)             xlen = L
         L     R10,ANCXLEN(,R2)            R10 = remaining = L
         SLR   R11,R11                     R11 = offset
WRINLP   LTR   R10,R10                     bytes left? (0 -> skip)
         BNP   WRINEND
         LR    R1,R10                      R1 = piece length
         C     R1,=A(MVCKMAX)
         BNH   WRINSZ
         LA    R1,MVCKMAX                  cap at 255
WRINSZ   LR    R0,R1                       save piece length
         L     R5,REQUBUF(,R8)             R5 = ubuf base (source B2)
         ALR   R5,R11
         LA    R4,ANCSTAGE(,R2)            R4 = &stage (dst B1)
         ALR   R4,R11
         LA    R3,MVCKK8                   R3 = source key 8
         DC    X'D9134000',X'5000'         MVCK 0(1,4),0(5),3
         ALR   R11,R0                      off += piece
         SLR   R10,R0                      remaining -= piece
         B     WRINLP
WRINEND  DS    0H
         XC    ANCRECB(4,R2),ANCRECB(R2)   reply_ecb = 0
         LA    R3,FNXFER
         ST    R3,ANCXFUNC(,R2)            xfunc = XFER
         ST    R7,ANCRASCB(,R2)            caller ASCB (POST target)
         LA    R3,STPEND
         ST    R3,ANCSTATE(,R2)            publish PENDING
*----------------------------------------------------------------------
*  Wake the STC: cross-AS branch POST via CVT0PT01, supervisor key 0 (the
*  exact @@xmpost.c sequence).  Only R9 survives the POST, so preserve our
*  registers with STM/LM into the anchor's 18-word save area; R9 carries the
*  save-area pointer.  R2 (anchor), R8 (A(req)), R14 (return) all restore
*  from the save area afterward, and the later WAIT (SVC 1) preserves R2-R14.
*----------------------------------------------------------------------
DOPOST   DS    0H                 POST/WAIT entry (ECHO + XFER share)
         LA    R13,ANCSAVE(,R2)   R13 -> 18-word POST save area
         STM   R14,R12,12(R13)    preserve regs across the POST
         LR    R9,R13             R9: the only reg POST preserves
         L     R10,=X'40000000'   POST completion code (0)
         LA    R11,ANCSECB(,R2)   A(server_ecb)
         O     R11,=X'80000000'   POST ECB-address convention bit
         LA    R12,PSTERR         POST error routine
         L     R13,ANCSASCB(,R2)  R13 = server ASCB (POST parameter)
         SLR   R15,R15
         L     R15,16(,R15)       R15 = CVT (absolute location 16)
         L     R15,CVT0PT01-CVTMAP(,R15)   POST branch entry
         BALR  R14,R15
         LR    R13,R9             restore save-area pointer
         LM    R14,R12,12(R13)    restore our registers
         B     PSTOK
PSTERR   DS    0H                 POST failed: STC ASCB gone
         LR    R13,R9
         LM    R14,R12,12(R13)
         B     PSTFAIL
PSTOK    DS    0H
*----------------------------------------------------------------------
*  WAIT for the reply on the key-0 CSA reply ECB, supervisor state, key 0.
*  ADR-0038 empirical unknown #1: Stage-0a's problem-state / key-8-stack-ECB
*  rule does NOT transfer -- the routine never leaves key 0.  On wake the STC
*  has either serviced us (state -> DONE) or, on quiesce, posted us to bail.
*----------------------------------------------------------------------
         LA    R3,ANCRECB(,R2)    A(reply_ecb)
         WAIT  1,ECB=(R3)
         CLC   ANCEYE(8,R2),=CL8'NSFVANCR'   anchor there?
         BNE   WGONE              freed while parked -> bail
         L     R3,ANCSTATE(,R2)
         C     R3,=A(STDONE)      serviced?
         BNE   WQUIES             no -> quiesce wake, bail
*----------------------------------------------------------------------
*  Normal reply: copy the STC's echo + served into the caller's block, release
*  the slot, decrement in-flight LAST (after every CSA write).
*----------------------------------------------------------------------
         L     R3,REQFUNC(,R8)    request function
         C     R3,=A(FNXFER)      XFER?
         BE    XFEROUT
*  ECHO: copy the echoed token (token+1) back into the caller's block.
         L     R3,ANCTOKEN(,R2)   echoed token
         ST    R3,REQTOKN(,R8)
         B     REPLYC
*----------------------------------------------------------------------
*  XFER read-out: MVCK the transformed staging (source key 0) back out to
*  the caller's ubuf (dst key 0), L = ANCXLEN bytes (reloaded from CSA --
*  it survived POST/WAIT), <= 255 bytes/piece, offset recomputed.  Piece
*  length saved in R0 to advance (MVCK not trusted to preserve R1).  The
*  write-out is key 0 in Stage-0b; M5-2 tightens the write-side key.
*----------------------------------------------------------------------
XFEROUT  DS    0H
         L     R10,ANCXLEN(,R2)   R10 = remaining = L
         SLR   R11,R11            R11 = offset
RDOTLP   LTR   R10,R10            bytes left? (0 -> skip)
         BNP   REPLYC
         LR    R1,R10             R1 = piece length
         C     R1,=A(MVCKMAX)
         BNH   RDOTSZ
         LA    R1,MVCKMAX         cap at 255
RDOTSZ   LR    R0,R1              save piece length
         L     R4,REQUBUF(,R8)    R4 = ubuf base (dst B1)
         ALR   R4,R11
         LA    R5,ANCSTAGE(,R2)   R5 = &stage (source B2)
         ALR   R5,R11
         SLR   R3,R3             R3 = source key 0
         DC    X'D9134000',X'5000'         MVCK 0(1,4),0(5),3
         ALR   R11,R0             off += piece
         SLR   R10,R0             remaining -= piece
         B     RDOTLP
REPLYC   DS    0H
         L     R3,ANCSERVD(,R2)
         ST    R3,REQSEQ(,R8)
         SLR   R3,R3
         ST    R3,REQRC(,R8)      caller rc = OK
         LA    R3,STFREE
         ST    R3,ANCSTATE(,R2)   DONE -> FREE
         L     R3,ANCINFL(,R2)
UDEC1    LR    R4,R3
         BCTR  R4,0
         CS    R3,R4,ANCINFL(R2)
         BNE   UDEC1
         SLR   R15,R15            R15 = RCOK
         BR    R14
*----------------------------------------------------------------------
*  Bail paths.
*----------------------------------------------------------------------
WQUIES   DS    0H                 quiesce wake: give inflight back
         LA    R3,STFREE          so the shutdown drain completes
         ST    R3,ANCSTATE(,R2)
         L     R3,ANCINFL(,R2)
UDEC2    LR    R4,R3
         BCTR  R4,0
         CS    R3,R4,ANCINFL(R2)
         BNE   UDEC2
         LA    R15,RCCORR
         ST    R15,REQRC(,R8)
         BR    R14
*
PSTFAIL  DS    0H                 POST failed: undo publish + uinc
         LA    R3,STFREE
         ST    R3,ANCSTATE(,R2)
         L     R3,ANCINFL(,R2)
UDEC3    LR    R4,R3
         BCTR  R4,0
         CS    R3,R4,ANCINFL(R2)
         BNE   UDEC3
         LA    R15,RCCORR
         ST    R15,REQRC(,R8)
         BR    R14
*
WGONE    DS    0H                 anchor freed: inflight is gone
         LA    R15,RCCORR
         ST    R15,REQRC(,R8)
         BR    R14
*
BADANC   DS    0H                 anchor bad (our caller): write rc
         LA    R15,RCCORR
         ST    R15,REQRC(,R8)
         BR    R14
*
SLOTBSY  DS    0H                 slot busy: write rc
         LA    R15,RCNOREQ
         ST    R15,REQRC(,R8)
         BR    R14
*
BADREQ   DS    0H                 null/foreign request: rc in R15 only
         LA    R15,RCINVAL
         BR    R14
*----------------------------------------------------------------------
         LTORG ,
         CVT   DSECT=YES,LIST=NO
         END   NSFVSVC
