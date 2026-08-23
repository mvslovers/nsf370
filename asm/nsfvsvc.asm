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
ANCXASID EQU   128                F    req_asid (client ASID)
ANCREAPD EQU   132                F    reaped (dead reqs reclaimed)
ANCSTAGE EQU   136                CSA staging buffer (2048)
ANCRQE   EQU   2184               M5-2a NSFRQE slot (64) ADR-0041
ANCRQEG  EQU   2248               RQE slot guard word (checked in C)
ANCSEPTR EQU   2252               A(STC private key-8 wake ECB)
*  NSFV_REQ field offsets (caller's block, R8 = A(req))
REQEYE   EQU   0                  CL4  "NSFV"
REQFUNC  EQU   4                  F    request function
REQTOKN  EQU   8                  F    in token / out echo (+1)
REQRC    EQU   12                 F    out rc
REQSEQ   EQU   16                 F    out served snapshot
REQUBUF  EQU   20                 A    XFER caller ubuf addr
REQULEN  EQU   24                 F    XFER bytes to move
REQPASC  EQU   28                 A    ORPHAN probe ASCB (in)
REQPASI  EQU   32                 F    ORPHAN probe ASID (in)
REQQSTA  EQU   36                 F    QUERY req_state (out)
REQQINF  EQU   40                 F    QUERY inflight (out)
REQQRPD  EQU   44                 F    QUERY reaped (out)
REQRQEI  EQU   48                 A    RQE: A(caller NSFRQE image)
*  state + rc constants (mirror nsfvsvc.h)
STFREE   EQU   0
STPEND   EQU   1
STDONE   EQU   2
STHELD   EQU   3                  STC declined (UNKNOWN client)
RCOK     EQU   0
RCINVAL  EQU   4
RCCORR   EQU   8
RCNOREQ  EQU   12
*  request functions + MVCK copy constants (mirror nsfvsvc.h)
FNECHO   EQU   1
FNXFER   EQU   2
FNORPH   EQU   3                  probe: stage + POST, no WAIT
FNQUERY  EQU   4                  probe: report anchor state
FNUNSTG  EQU   5                  probe: release a held slot
FNRQE    EQU   6                  M5-2a: carry an NSFRQE (ADR-0041)
*  ASCB field the Stage-0c guard needs (ADR-0040): the caller ASID.  R7 is
*  A(caller ASCB), set by the SVC FLIH, so the ASID comes from the control
*  block and NOT from the request -- a client cannot forge its identity.
ASCBASID EQU   36                 ASCBASID halfword (ASCB+X'24')
XFCHUNK  EQU   2048               max ulen moved per SVC call
RQELEN   EQU   64                 frozen NSFRQE size (one MVCK piece)
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
*  Stage-0c probe verbs that take NO slot and change no in-flight state:
*  QUERY reports the anchor's request state, UNSTAGE gives back a slot the
*  STC deliberately did not release (ADR-0040 8).  Both have to work while
*  the slot is BUSY -- that is the state the probe needs to observe -- so
*  they branch out ahead of the slot-take below.
*----------------------------------------------------------------------
         L     R3,REQFUNC(,R8)    request function
         C     R3,=A(FNQUERY)
         BE    DOQUERY
         C     R3,=A(FNUNSTG)
         BE    DOUNSTG
*----------------------------------------------------------------------
*  Take the one request slot (single-client sequential probe: reject a
*  concurrent client rather than corrupt the slot).  Reject BEFORE the
*  in-flight increment, so a rejected caller leaves inflight untouched.
*  Any non-FREE state is busy -- including HELD (ADR-0040 6).
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
         C     R3,=A(FNORPH)               ORPHAN (probe)?
         BE    ORPHIN                      yes -> stage probe identity
         C     R3,=A(FNRQE)                RQE (M5-2a)?
         BE    RQEIN                       yes -> stage ubuf + NSFRQE
*  ECHO: stage the token.  Set xfunc = ECHO so an ECHO after an XFER is not
*  misdispatched by the STC (which switches on the staged xfunc).
         XC    ANCRECB(4,R2),ANCRECB(R2)   reply_ecb = 0
         L     R3,REQTOKN(,R8)             read caller token
         ST    R3,ANCTOKEN(,R2)            stage token
         LA    R3,FNECHO
         ST    R3,ANCXFUNC(,R2)            xfunc = ECHO
         ST    R7,ANCRASCB(,R2)            caller ASCB (POST target)
         LH    R3,ASCBASID(,R7)            caller ASID (ASCB+X'24')
         N     R3,=X'0000FFFF'             halfword, no sign extension
         ST    R3,ANCXASID(,R2)            stage the ASID (Stage-0c)
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
         LH    R3,ASCBASID(,R7)            caller ASID (ASCB+X'24')
         N     R3,=X'0000FFFF'             halfword, no sign extension
         ST    R3,ANCXASID(,R2)            stage the ASID (Stage-0c)
         LA    R3,STPEND
         ST    R3,ANCSTATE(,R2)            publish PENDING
         B     DOPOST                      EXPLICIT: see the note below
*----------------------------------------------------------------------
*  Every staging block above ends with an EXPLICIT B DOPOST, including the
*  one immediately ahead of DOPOST.  Before Stage-0c the XFER block simply
*  fell through into the POST block, and inserting ORPHAN between the two
*  silently redirected XFER into it -- staging the ORPHAN identity (zeros
*  for an XFER caller), which the guard then correctly called UNKNOWN and
*  the client waited forever for a reply that must not be sent.  The
*  assembler cannot see that, a green link cannot see it, and neither can
*  a probe that never issues an XFER: it took a live TSTUBUF hang.  Do not
*  "clean up" these branches.
*----------------------------------------------------------------------
*  ORPHAN (Stage-0c probe only, ADR-0040 8).  Stages the request exactly as
*  ECHO does, EXCEPT that the client identity comes from the request block
*  (pascb/pasid) and is stored VERBATIM -- that is the whole point: the probe
*  hands the STC an identity naming an address space that is not there, so the
*  guard has something to classify DEAD.  The caller then returns WITHOUT
*  waiting (see PSTOK), so the in-flight decrement is skipped by construction,
*  which is exactly what a client that died mid-request leaves behind.
*----------------------------------------------------------------------
ORPHIN   DS    0H
         XC    ANCRECB(4,R2),ANCRECB(R2)   reply_ecb = 0
         L     R3,REQTOKN(,R8)             read caller token
         ST    R3,ANCTOKEN(,R2)            stage token
         LA    R3,FNECHO
         ST    R3,ANCXFUNC(,R2)            transform stays ECHO
         L     R3,REQPASC(,R8)             probe ASCB
         ST    R3,ANCRASCB(,R2)            stored VERBATIM
         L     R3,REQPASI(,R8)             probe ASID
         ST    R3,ANCXASID(,R2)            stored VERBATIM
         LA    R3,STPEND
         ST    R3,ANCSTATE(,R2)            publish PENDING
         B     DOPOST                      EXPLICIT: never fall through
*----------------------------------------------------------------------
*  RQE write-in (M5-2a, ADR-0041).  Carries a real request across: the user
*  data AND the 64-byte NSFRQE that describes it.  Two moves, in this order:
*
*    1. ubuf -> stage[], clamped L = min(ulen, XFCHUNK), exactly as XFER.
*       The clamp is what the dispatcher is later told through the NSFRQE's
*       ulen, so the op reports the count that actually crossed (ADR-0041 2).
*    2. the caller's NSFRQE image -> ANCRQE, one 64-byte MVCK piece (64 <=
*       MVCKMAX, so no loop is needed).
*
*  Both are source key 8 -> dst key 0, raw D9: as370 mis-assembles the MVCK
*  mnemonic (drops the R1/R3 registers), see tstmvck.c.  Piece length rides
*  in R0 because MVCK is not trusted to preserve R1.
*----------------------------------------------------------------------
RQEIN    DS    0H
         L     R0,REQULEN(,R8)             R0 = ulen
         C     R0,=A(XFCHUNK)              > staging size?
         BNH   RQISTX
         L     R0,=A(XFCHUNK)              clamp to staging size
RQISTX   ST    R0,ANCXLEN(,R2)             xlen = L (kept across WAIT)
         L     R10,ANCXLEN(,R2)            R10 = remaining = L
         SLR   R11,R11                     R11 = offset
RQINLP   LTR   R10,R10                     bytes left? (0 -> skip)
         BNP   RQINEND
         LR    R1,R10                      R1 = piece length
         C     R1,=A(MVCKMAX)
         BNH   RQINSZ
         LA    R1,MVCKMAX                  cap at 255
RQINSZ   LR    R0,R1                       save piece length
         L     R5,REQUBUF(,R8)             R5 = ubuf base (source B2)
         ALR   R5,R11
         LA    R4,ANCSTAGE(,R2)            R4 = &stage (dst B1)
         ALR   R4,R11
         LA    R3,MVCKK8                   R3 = source key 8
         DC    X'D9134000',X'5000'         MVCK 0(1,4),0(5),3
         ALR   R11,R0                      off += piece
         SLR   R10,R0                      remaining -= piece
         B     RQINLP
RQINEND  DS    0H
         L     R5,REQRQEI(,R8)             R5 = A(caller RQE) src B2
         LTR   R5,R5                       no image supplied?
         BZ    RQINPUB                     then stage nothing
         LA    R4,ANCRQE(,R2)              R4 = &anchor.rqe (dst B1)
         LA    R1,RQELEN                   R1 = 64 (one piece)
         LA    R3,MVCKK8                   R3 = source key 8
         DC    X'D9134000',X'5000'         MVCK 0(1,4),0(5),3
RQINPUB  DS    0H
         XC    ANCRECB(4,R2),ANCRECB(R2)   reply_ecb = 0
         LA    R3,FNRQE
         ST    R3,ANCXFUNC(,R2)            xfunc = RQE
         ST    R7,ANCRASCB(,R2)            caller ASCB (POST target)
         LH    R3,ASCBASID(,R7)            caller ASID (ASCB+X'24')
         N     R3,=X'0000FFFF'             halfword, no sign extension
         ST    R3,ANCXASID(,R2)            stage the ASID (Stage-0c)
         LA    R3,STPEND
         ST    R3,ANCSTATE(,R2)            publish PENDING
         B     DOPOST                      EXPLICIT: never fall through
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
*  POST target: the STC's published key-8 private ECB if it has one, else
*  the key-0 CSA server_ecb.  The executive WAITs from problem state, where a
*  key-0 ECB is a documented abend (S047 / X'201'); the Stage-0 probe STC
*  publishes nothing and keeps its supervisor WAIT on the CSA ECB.
         L     R11,ANCSEPTR(,R2)  A(STC private key-8 ECB)
         LTR   R11,R11            published?
         BNZ   PSTECBX            yes -> post that one
         LA    R11,ANCSECB(,R2)   no  -> fall back to CSA server_ecb
PSTECBX  DS    0H
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
*  ORPHAN leaves here: the STC is awake, the request is in flight, and this
*  caller neither waits for the reply nor gives the in-flight count back.
         L     R3,REQFUNC(,R8)    request function
         C     R3,=A(FNORPH)
         BE    ORPHRET
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
         C     R3,=A(FNRQE)       RQE (M5-2a)?
         BE    RQEOUT
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
*  RQE read-out (M5-2a, ADR-0041).  The mirror of RQEIN, in the same order:
*  the transformed staging back out to the caller's ubuf (read-direction
*  data), then the 64-byte NSFRQE image back to the caller's block.
*
*  The image is copied WHOLE, but only the STC's result fields differ: the
*  STC's copy-out writes retcode/errno_/apptok/p1/p2/p3 into the slot and
*  nothing else (ADR-0041 4), so every caller-owned field still holds the
*  value the caller sent.  Which fields are actually APPLIED to the caller's
*  live NSFRQE is decided one level up, in C, by nsfreqx_result_in -- that is
*  where the field policy is host-tested, and it is deliberately not
*  duplicated here in assembler.
*
*  TWO KEYS, ONE SENTENCE APART -- do not conflate them.  The SOURCE key is 0
*  because the staging buffer and the slot are key-0 CSA; that half is correct
*  and harmless.  The HAZARD is the DESTINATION: both moves store into
*  CALLER-SUPPLIED addresses (ubuf, and now rqeimg) while running under PSW
*  key 0, so the hardware does NOT check them against the caller's key.  That
*  window stays open and is M5-2b, not this step -- and after M5-2a it has TWO
*  destinations, not one.
*----------------------------------------------------------------------
RQEOUT   DS    0H
         L     R10,ANCXLEN(,R2)   R10 = remaining = L
         SLR   R11,R11            R11 = offset
RQOTLP   LTR   R10,R10            bytes left? (0 -> skip)
         BNP   RQOTRQE
         LR    R1,R10             R1 = piece length
         C     R1,=A(MVCKMAX)
         BNH   RQOTSZ
         LA    R1,MVCKMAX         cap at 255
RQOTSZ   LR    R0,R1              save piece length
         L     R4,REQUBUF(,R8)    R4 = ubuf base (dst B1)
         ALR   R4,R11
         LA    R5,ANCSTAGE(,R2)   R5 = &stage (source B2)
         ALR   R5,R11
         SLR   R3,R3              R3 = source key 0
         DC    X'D9134000',X'5000'         MVCK 0(1,4),0(5),3
         ALR   R11,R0             off += piece
         SLR   R10,R0             remaining -= piece
         B     RQOTLP
RQOTRQE  DS    0H
         L     R4,REQRQEI(,R8)    R4 = A(caller NSFRQE) dst B1
         LTR   R4,R4              no image supplied?
         BZ    REPLYC             then nothing to give back
         LA    R5,ANCRQE(,R2)     R5 = &anchor.rqe (source B2)
         LA    R1,RQELEN          R1 = 64 (one piece)
         SLR   R3,R3              R3 = source key 0
         DC    X'D9134000',X'5000'         MVCK 0(1,4),0(5),3
         B     REPLYC             EXPLICIT: never fall through
*----------------------------------------------------------------------
*  Stage-0c probe handlers (ADR-0040 8).  Probe-only: not part of the M5-2
*  transport.  ORPHRET is the no-WAIT return; DOQUERY reports the anchor's
*  state to a client that cannot read CSA itself; DOUNSTG releases a slot the
*  STC declined to release, so the probe leaves no in-flight count behind and
*  the STC still stops clean.
*----------------------------------------------------------------------
ORPHRET  DS    0H                 orphan: no WAIT, no decrement
         SLR   R3,R3
         ST    R3,REQRC(,R8)      caller rc = OK
         SLR   R15,R15
         BR    R14
*
DOQUERY  DS    0H                 report state (no slot, no change)
         L     R3,ANCSTATE(,R2)
         ST    R3,REQQSTA(,R8)
         L     R3,ANCINFL(,R2)
         ST    R3,REQQINF(,R8)
         L     R3,ANCREAPD(,R2)
         ST    R3,REQQRPD(,R8)
         L     R3,ANCSERVD(,R2)
         ST    R3,REQSEQ(,R8)
         SLR   R3,R3
         ST    R3,REQRC(,R8)      caller rc = OK
         SLR   R15,R15
         BR    R14
*
*  UNSTAGE: FREE the slot and give one in-flight count back, but never take
*  the count below zero -- the STC may already have reaped this request.
DOUNSTG  DS    0H
         L     R3,ANCSTATE(,R2)
         LTR   R3,R3              already FREE?
         BZ    UNSTRC             yes -> nothing to give back
         SLR   R3,R3
         ST    R3,ANCSTATE(,R2)   -> FREE
         L     R3,ANCINFL(,R2)
UNSTLP   LTR   R3,R3              never below zero
         BZ    UNSTRC
         LR    R4,R3
         BCTR  R4,0
         CS    R3,R4,ANCINFL(R2)
         BNE   UNSTLP
UNSTRC   DS    0H
         SLR   R3,R3
         ST    R3,REQRC(,R8)      caller rc = OK
         SLR   R15,R15
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
