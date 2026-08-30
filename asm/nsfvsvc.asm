*----------------------------------------------------------------------
* nsfvsvc.asm -- M5 Stage-0a' SVC cross-AS probe: the private SVC
* routine.
*
* ADR-0038 (supersedes ADR-0036's SSI transport).  This is the routine
* a
* dynamically installed private SVC dispatches, in the CALLER's address
* space, to hand a request across to the probe STC (NSFV) and back.  It
* is
* the SVC analogue of Stage-0a's SSI router (retired; ADR-0036) -- same
* anchor,
* same __xmpost cross-AS wake, same in-flight discipline -- but reached
* by
* SVC dispatch instead of IEFSSREQ, so it serves an UNAUTHORIZED
* problem-
* state client (no APF): the SVC is the APF-free
* unauthorized->authorized
* transition (ADR-0038).  No NSFRQE, no socket, no protocol: it
* round-trips
* a 32-bit token, staged on an empty payload before M5-2 rides the real
* request over this transport.
*
* Written in assembler (Mike's call), NOT cc370 C: an SVC routine has
* no C
* runtime (the cc370 prologue's @@CRTGET wants a per-TCB CRT an
* arbitrary
* caller's TCB may not have usable), and the register-in/out convention
* is
* native to assembler.  Modelled on the CBT/mvs38j-ip stolen-slot
* transport
* SVC (STCPSVC) and Type-3 SVC shape (igc0024e.asm), plus the libc370
* branch
* POST (@@xmpost.c).
*
* NAMING (Mike's question).  A STOLEN-slot SVC routine takes an
* ARBITRARY
* CSECT name, NOT the IGCnnn scheme: the ancestor's transport SVC is
* CSECT
* "STCPSVC" (cbt571/PDS/STCPSVC), while its SYSGEN-installed auth SVC
* is
* "IGC0024E" (SVC 245).  The IGCnnn scheme is only for SVCs MVS loads
* BY NAME
* (SYSGEN / the standard SVC loader).  We steal the SVCTABLE slot and
* install
* a RESIDENT CSA entry point directly (STCPSVC0), so the loader is
* bypassed and
* the name is free -- NSFVSVC follows STCPSVC's precedent.
*
* RENT: entered concurrently from many address spaces/tasks -- no
* writable
* statics (the NSFVANCH word is patched ONCE by the STC before the slot
* is
* stolen, then read-only; the probe's single-client-sequential model
* makes the
* shared anchor scratch safe -- ADR-0038).  project.toml:
* entry=NSFVSVC,
* startup=false, ac=1; __loadhi'd into CSA by the STC.
*
* Entry (Type-3 SVC, set by the SVC FLIH -- STCPSVC / igc0024e.asm):
*   supervisor state, PSW key 0, ENABLED
*
*   reg  set by the FLIH to        still valid at DOPOST / RQEOUT?
*   ---  ------------------------ 
*   ------------------------------------------
*   R1   A(NSFV_REQ), caller AS    no -- copied to R8 at entry; use R8
*   R4   A(TCB)                    NO -- DESTROYED: RQEIN/XFERIN use it
*   as the
*                                  MVCK destination pointer (LA
*                                  R4,ANCSTAGE
*                                  and LA R4,ANCRQE).  Use PSATOLD
*                                  instead.
*   R5   A(SVRB)                   NO -- DESTROYED: RQEIN/XFERIN use it
*   as the
*                                  MVCK source pointer (L R5,REQUBUF
*                                  and
*                                  L R5,REQRQEI).  Use TCBRBP instead.
*   R6   A(entry point) = base     yes (and it is our USING base --
*   preserve
*                                  it across the POST or nothing
*                                  addresses)
*   R7   A(caller ASCB)            REASSIGNED at CLAIMOK: the ASCB and
*   ASID
*                                  are recorded in the slot there,
*                                  while R7
*                                  still holds them, and R7 then
*                                  becomes
*                                  A(our slot) for the rest of the
*                                  routine.
*                                  It is SAVED ACROSS THE POST like the
*                                  anchor base -- nothing afterwards
*                                  can
*                                  re-derive which slot is ours,
*                                  because the
*                                  claim is long past and a fresh scan
*                                  would
*                                  find a DIFFERENT free slot.
*   R13  issuer R13                overwritten (POST parameter); the
*   FLIH
*                                  gives the issuer its own R13 back
*   R14  return address            yes -- must be preserved across the
*   POST
*
*   THE RULE, and it is why this is a table and not prose: a register
*   the FLIH
*   set at entry is trustworthy at DOPOST/RQEOUT ONLY IF no staging
*   block has
*   since used it as scratch.  Both registers the ancestor's convention
*   names
*   as useful -- R4 (TCB) and R5 (SVRB) -- are destroyed, and between
*   them they
*   exhaust the registers anyone would reach for.  That is not two
*   coincidences.  It cost M5-2b1 one debugging round (R4, the TCBPKF
*   read)
*   and M5-2b2 another (R5, the save-area address, which put the save
*   area
*   inside the CLIENT's storage).  The trustworthy sources are
*   absolute:
*   PSATOLD for the TCB, and TCBRBP off it for the RB/SVRB.
* Exit:  BR R14.  Per STCPSVC: "R0, R1, R15 are the only regs returned
* to the
*   issuer; R2-R14 are restored by the system."  We set R15 = rc and
*   also write
*   the full result (echo/seq/rc) into the caller's NSFV_REQ block,
*   which is
*   therefore authoritative and independent of the register-return
*   path.
*
* Column-71 discipline (CLAUDE.md 3): instruction-line comments stay
* short;
* rationale lives in these full-width '*' blocks.  CS/LM operands are
* D(B).
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
*  NSFV_ANCHOR field offsets -- MIRROR of include/nsfvsvc.h (guarded
*  there
*  by NSF_SIZE_ASSERT at cross-compile).
*----------------------------------------------------------------------
*  Global header (M5-2b3: the anchor is a header + a 64-slot array).
ANCEYE   EQU   0                  CL8  "NSFVANCR"
ANCVER   EQU   8                  F    version -- checked below
ANCFLAG  EQU   12                 F    ACTIVE = X'80000000'
ANCSECB  EQU   16                 F    server_ecb (fallback WAIT)
ANCSASCB EQU   20                 A    server_ascb (POST target)
ANCINFL  EQU   24                 F    inflight
ANCSERVD EQU   28                 F    served
ANCREAPD EQU   32                 F    reaped (dead reqs reclaimed)
ANCSEPTR EQU   36                 A    A(STC private key-8 wake ECB)
ANCNSLOT EQU   40                 F    nslots -- THE SCAN'S BOUND
ANCEXH   EQU   44                 F    exhausted (ENOBUFS count)
ANCCOLL  EQU   48                 F    collisions (contended claims)
ANCRSV0  EQU   52                 F    reserved -- header slack
ANCSLOTS EQU   56                 the slot array base
*  Per slot, addressed off R7 (the slot base).  ADR-0042 5.
SLSTATE  EQU   0                  F    req_state -- THE CS CLAIM WORD
SLTOKEN  EQU   4                  F    req_token
SLRECB   EQU   8                  F    reply_ecb (this client's WAIT)
SLRASCB  EQU   12                 A    req_ascb (caller ASCB)
SLASID   EQU   16                 F    req_asid (client ASID)
SLXFUNC  EQU   20                 F    transform (ECHO/XFER/RQE)
SLXLEN   EQU   24                 F    bytes staged this chunk
SLRQE    EQU   28                 NSFRQE image (64) ADR-0041
SLRQEG   EQU   92                 CL4  slot guard word (checked in C)
SLSTAGE  EQU   96                 CSA staging buffer (2048)
SLOTLEN  EQU   2144               one slot -- the scan's stride
*  ANCSAVE is GONE (issue #61).  It was dead as a save area from
*  M5-2b2, when
*  the POST save area moved into the SVRB's RBEXSAVE, and was kept only
*  because removing it would shift every later field.  b3 moves the
*  layout
*  anyway, so it comes out here for free -- and b2's five-word
*  self-check
*  goes with it: one-time evidence, collected, recorded in ADR-0038. 
*  Carrying
*  five stores and three CLCs per request forever to re-prove a settled
*  fact
*  is not a trade worth making on the hot path.
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
REQSLOT  EQU   52                 F    in: probe idx; out: claimed
REQSEXP  EQU   56                 F    SLOT probe: expected state
REQSNEW  EQU   60                 F    SLOT probe: state to set
*  state + rc constants (mirror nsfvsvc.h)
STFREE   EQU   0
STPEND   EQU   1
STDONE   EQU   2
STHELD   EQU   3                  STC declined (UNKNOWN client)
STCLAIM  EQU   4                  claimed, staging in progress (b3)
RCOK     EQU   0
RCINVAL  EQU   4
RCCORR   EQU   8
RCNOREQ  EQU   12                 named slot busy (probe paths)
RCNOBUF  EQU   16                 POOL FULL -> ENOBUFS (ADR-0042 7)
*  Anchor layout version.  MUST match ANCVER in the anchor the STC
*  built.
*  NSFVSVC is a SEPARATE load module from NSFS and a mid-chain deploy
*  failure
*  silently keeps the previous one (CLAUDE.md 5) -- a stale routine
*  against a
*  moved layout is a wild CSA store, not a wrong answer.  Two
*  instructions
*  turn that into RCCORR.  Bump with every layout move or it is
*  decorative.
ANCVERNO EQU   3                  NSFV_ANCHOR_VER
*  request functions + MVCK copy constants (mirror nsfvsvc.h)
FNECHO   EQU   1
FNXFER   EQU   2
FNORPH   EQU   3                  RETIRED (M5-2c2 b): rejected
FNQUERY  EQU   4                  probe: report anchor state
FNUNSTG  EQU   5                  probe: release a held slot
FNRQE    EQU   6                  M5-2a: carry an NSFRQE (ADR-0041)
FNSLOT   EQU   7                  probe: CS one named slot's state
*  ASCB field the Stage-0c guard needs (ADR-0040): the caller ASID.  R7
*  is
*  A(caller ASCB), set by the SVC FLIH, so the ASID comes from the
*  control
*  block and NOT from the request -- a client cannot forge its
*  identity.
ASCBASID EQU   36                 ASCBASID halfword (ASCB+X'24')
XFCHUNK  EQU   2048               max ulen moved per SVC call
RQELEN   EQU   64                 frozen NSFRQE size (one MVCK piece)
MVCKMAX  EQU   255                bytes per MVCK piece
MVCKK8   EQU   X'80'              MVCK source key 8
MVCMAX   EQU   256                bytes per MVC piece (write-out)
*  M5-2b1: the caller's storage key, for the write-out SPKA window.
*  PSATOLD (PSA+X'21C') is A(the current TCB) -- the CALLER's TCB,
*  since an
*  SVC routine runs under the issuing task.  The ancestor
*  (igc0024e.asm) does
*  document R4 = A(TCB) at SVC entry, but R4 is loop scratch in RQEIN
*  long
*  before the write-out runs and the POST save/restore preserves the
*  CLOBBERED
*  value, so R4 is NOT the TCB by the time RQEOUT is reached -- PSATOLD
*  is,
*  always, and it is what libc370 itself uses (src/clib/getmain.c:40).
*  TCBPKF (TCB+X'1C') holds the key in its HIGH nibble with the low
*  nibble
*  defined zero (IKJTCB: TCBFLAG EQU X'F0', TCBZERO EQU X'0F'), which
*  is
*  exactly the byte SPKA wants -- SPKA takes bits 24-27 of its operand
*  address
*  -- so it is used with no shifting, the same way @@super.c does.
PSATOLD  EQU   540                PSA+X'21C' = A(current TCB)
*  M5-2b2: the POST save area is PER-INVOCATION, in the SVRB.
*
*  READ, NOT GUESSED.  SYS1.AMODGEN(IHARB)/(IKJTCB) read live and the
*  offsets
*  computed by IFOX00 from the macros (jobs RBOFF/RBOFF2) -- these
*  DSECTs are
*  conditional-assembly and hand-counting them is the one mistake a
*  RENT
*  routine with no writable statics cannot afford:
*    RBEXSAVE DS 0CL48  "EXTENDED SAVE AREA FOR SVC ROUTINES
*    (SVRB-BOTH)"
*    RBEXSAVE-RBBASIC = X'60'   L'RBEXSAVE = X'30'   RBSIZE-RBBASIC = 8
*    RBBASIC-RBPRFX   = X'40'   TCBRBP-TCB = 0
*
*  A(SVRB) COMES FROM TCBRBP, NOT FROM R5.  The FLIH does set R5 =
*  A(SVRB) --
*  but R5 is the MVCK SOURCE POINTER in RQEIN and XFERIN, so by the
*  time
*  DOPOST is reached it holds A(caller ubuf) or A(caller NSFRQE), NOT
*  the
*  SVRB.  This is b1's PSATOLD-over-R4 finding exactly, one register
*  over, and
*  it is what broke the first attempt at this step: LA
*  R13,RBEXSAVE(,R5) put
*  the save area inside the CLIENT's storage, where the store of the
*  caller
*  ASCB landed on the test's own variables.  Measured: R5 = X'000991EC'
*  (the
*  caller's image) while TCBRBP = X'009DE5F0' (LSQA), and RBSIZE read
*  off
*  TCBRBP is 28 doublewords = 224 bytes, a sensible RB; off R5 it was
*  garbage.
*
*  TWELVE WORDS, NOT EIGHTEEN.  L'RBEXSAVE is 48 bytes; the old shared
*  block
*  was an 18-word standard save area (STM R14,R12,12(R13) = 72 bytes
*  from
*  offset 12) and does not fit.  Only FOUR registers are live across
*  the POST
*  -- R2 (anchor), R6 (OUR CSECT BASE, USING NSFVSVC,R6), R8 (A(req))
*  and R14
*  (return) -- so four stores of 16 bytes are used instead of an STM of
*  11.
*  Deliberately minimal: the less of this area we touch, the smaller
*  the
*  question of who owns the rest becomes.  R5/R10/R11/R12 are reloaded
*  after
*  the POST, R13 is never restored, and R9 carries the area pointer
*  because R9
*  is the one register the branch POST preserves.
*
*  MEASURED, NOT ASSUMED: a canary stamped in this area survives BOTH
*  the
*  branch POST and the WAIT -- the three-point self-check below records
*  it and
*  TSTRQXF asserts it.  The predicted failure (that RBEXSAVE would be
*  usable
*  while the routine runs but not across a suspension) did NOT occur.
RBEXSAVE EQU   96                 SVRB+X'60' (IHARB, RBSECPTR-relative)
RBEXSVLN EQU   48                 L'RBEXSAVE -- 12 words, NOT 18
TCBRBP   EQU   0                  TCB+0 = A(current RB) = A(SVRB)
TCBPKF   EQU   28                 TCB+X'1C'  = task storage key
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
*  LAYOUT VERSION (M5-2b3).  NSFVSVC is a separate load module from
*  NSFS and
*  a mid-chain deploy failure silently keeps the previous one, so a
*  stale
*  routine against a moved layout is reachable -- and it would stride
*  the
*  scan by the wrong slot length or walk off the end of the allocation.
*  Two instructions turn an IPL-class wild store into a clean RCCORR.
         L     R3,ANCVER(,R2)     anchor layout version
         C     R3,=A(ANCVERNO)    the one we were built for?
         BNE   BADANC             stale routine or stale STC
         TM    ANCFLAG(R2),X'80'  ACTIVE?  (X'80000000' high byte)
         BNO   BADANC             quiescing -> CORRUPT
*----------------------------------------------------------------------
*  Probe verbs that NAME a slot instead of claiming one, and change no
*  in-flight state: QUERY reports one slot's state, UNSTAGE gives back
*  a
*  slot the STC deliberately did not release (ADR-0040 8), SLOT
*  compare-and-
*  swaps one slot's state so a test can pre-claim (ADR-0042).  All
*  three
*  have to work while a slot is BUSY -- that is the state the probe
*  needs to
*  observe -- so they branch out ahead of the claim below.
*----------------------------------------------------------------------
         L     R3,REQFUNC(,R8)    request function
*  RETIRED VERB (M5-2c2 stage b).  FNORPH stored a request-supplied
*  identity into the slot verbatim: a forged identity taken from an
*  unauthorised caller, and the half of obligation #4 this discharges.
*  Rejected HERE, ahead of the claim, so a retired verb costs NO slot
*  and NO in-flight count -- true by position, not by argument.
*  The code is NOT reused.  Rejecting it by name also beats deleting
*  the test alone, which would let FNORPH fall through to the ECHO
*  default and be serviced as an ordinary ECHO.
         C     R3,=A(FNORPH)      retired -> reject, claim nothing
         BE    BADFUNC
         C     R3,=A(FNQUERY)
         BE    DOQUERY
         C     R3,=A(FNUNSTG)
         BE    DOUNSTG
         C     R3,=A(FNSLOT)
         BE    DOSLOT
*----------------------------------------------------------------------
*  CLAIM A SLOT (M5-2b3, ADR-0042).  CS each slot's OWN state word from
*  FREE to CLAIMED; first success wins.  A failed CS means another
*  client
*  took that slot, so the walk ADVANCES -- it never retries the same
*  slot,
*  because there is nothing to retry: the slot is gone.
*
*  ABA-FREE BY CONSTRUCTION: the location being compared IS the
*  resource.
*  There is no head pointer whose value could return to a previous
*  state
*  while the thing it names changed underneath it, which is why this
*  design
*  has no free list -- see asm/nsfxq.asm's header, which forbids
*  exactly the
*  per-element pop a free list would need.
*
*  THE BOUND COMES FROM THE ANCHOR (ANCNSLOT), NOT FROM AN EQU HERE. 
*  The
*  party that knows how much storage exists is the party that allocated
*  it.
*  With the version check above this is belt and braces, and both are
*  two
*  instructions.
*
*  NO INDEX MULTIPLY: the cursor is a POINTER advanced by LA, which is
*  why
*  the slot is not padded to a power of two (2144 fits an LA
*  displacement).
*  R9 counts the index alongside it, purely so the caller can be told
*  which
*  slot served it -- the live reuse and skip checks are statements
*  about
*  that number.
*
*  THIS LOOP DOES NOT RUN UNDER A BORROWED KEY.  A claim needs an
*  interlocked compare; that is serialisation, not protection.  MOVEOUT
*  stays the only block in this routine that leaves key 0.
*
*  Exhaustion is rejected BEFORE the in-flight increment, so a caller
*  that
*  got no slot leaves inflight untouched.
*----------------------------------------------------------------------
         LA    R10,ANCSLOTS(,R2)  R10 = &slots[0]
         L     R11,ANCNSLOT(,R2)  R11 = nslots (the allocator's count)
         LTR   R11,R11            a pool at all?
         BNP   POOLFUL            no -> ENOBUFS, never scan
         SLR   R9,R9              R9 = slot index
CLAIMLP  DS    0H
         SLR   R3,R3              R3 = FREE (the comparand)
         LA    R4,STCLAIM         R4 = CLAIMED (swap-in)
         CS    R3,R4,SLSTATE(R10) free ? take it : R3 = actual
         BE    CLAIMOK            ours
*  CONTENDED (M5-2b4).  The compare failed, so this slot was NOT FREE
*  at the
*  instant we compared -- somebody else's claim is there.  Count it and
*  walk
*  on.  R3 (the value CS handed back) and R4 (the swap-in constant) are
*  both
*  dead here and both re-set at the top of the next iteration; R7 is
*  NOT
*  touched -- it still holds the FLIH's caller ASCB until CLAIMOK
*  records it.
*  Plain L/LA/ST on purpose: see "COLLISIONS" in include/nsfvsvc.h. 
*  This is
*  the routine's one hot loop and the counter is a diagnostic that may
*  under-report; do not turn it into the CS loop POOLFUL uses.
         L     R3,ANCCOLL(,R2)    contended-claim counter
         LA    R3,1(,R3)
         ST    R3,ANCCOLL(,R2)
CLAIMNX  DS    0H
         LA    R9,1(,R9)          next index
         LA    R10,SLOTLEN(,R10)  next slot (pointer walk)
         BCT   R11,CLAIMLP
         B     POOLFUL            every slot taken -> ENOBUFS
CLAIMOK  DS    0H
         ST    R9,REQSLOT(,R8)    tell the caller which slot
*  Record the identity while R7 is still the FLIH's caller ASCB -- it
*  comes
*  from the control block, never from the request, so it cannot be
*  forged
*  (ADR-0040).  R7 becomes the SLOT BASE immediately afterwards.
         ST    R7,SLRASCB(,R10)   caller ASCB (POST target)
         LH    R3,ASCBASID(,R7)   caller ASID (ASCB+X'24')
         N     R3,=X'0000FFFF'    halfword, no sign extension
         ST    R3,SLASID(,R10)    stage the ASID (Stage-0c)
         LR    R7,R10             R7 = A(our slot) from here on
*----------------------------------------------------------------------
*  Mark in-flight (shutdown clears ACTIVE and drains this to zero
*  before it
*  frees the CSA).  CS loop = the in-flight __uinc of Stage-0a's SSI
*  router.
*----------------------------------------------------------------------
         L     R3,ANCINFL(,R2)
UINCLP   LR    R4,R3
         LA    R4,1(,R4)
         CS    R3,R4,ANCINFL(R2)
         BNE   UINCLP
*----------------------------------------------------------------------
*  Dispatch on req.func.  ECHO stages the token; XFER first copies the
*  caller's ubuf into the CSA staging buffer (write-in), then stages
*  xlen.
*  Both publish req_state = PENDING LAST (after every other field), so
*  an
*  STC wake never services a half-formed slot; and zero the reply ECB
*  first
*  so a stale post cannot false-wake the WAIT.
*----------------------------------------------------------------------
         L     R3,REQFUNC(,R8)             request function
         C     R3,=A(FNXFER)               XFER?
         BE    XFERIN                      yes -> write-in + xlen
         C     R3,=A(FNRQE)                RQE (M5-2a)?
         BE    RQEIN                       yes -> stage ubuf + NSFRQE
*  ECHO: stage the token.  Set xfunc = ECHO so an ECHO after an XFER is
*  not
*  misdispatched by the STC (which switches on the staged xfunc).
         XC    SLRECB(4,R7),SLRECB(R7)     reply_ecb = 0
         L     R3,REQTOKN(,R8)             read caller token
         ST    R3,SLTOKEN(,R7)             stage token
         LA    R3,FNECHO
         ST    R3,SLXFUNC(,R7)             xfunc = ECHO
         LA    R3,STPEND
         ST    R3,SLSTATE(,R7)             publish PENDING
         B     DOPOST
*----------------------------------------------------------------------
*  XFER write-in.  Clamp L = min(ulen, XFCHUNK): a > chunk ulen would
*  run
*  MVCK off the end of stage[] (the last anchor field) into adjacent
*  CSA --
*  an IPL-class overrun.  Stage xlen = L (in CSA, survives POST/WAIT,
*  reused
*  for read-out).  Then MVCK the caller's ubuf (source key 8) into
*  stage
*  (dst key 0), <= 255 bytes/piece -- raw D9, since as370 mis-assembles
*  the
*  MVCK mnemonic (drops the R1/R3 registers); see tstmvck.c.  The piece
*  length is saved in R0 (MVCK is not trusted to preserve R1) and used
*  to
*  advance, so the loop never depends on a register MVCK may clobber.
*----------------------------------------------------------------------
XFERIN   DS    0H
         L     R0,REQULEN(,R8)             R0 = ulen
         C     R0,=A(XFCHUNK)              > staging size?
         BNH   XFISTX
         L     R0,=A(XFCHUNK)              clamp to staging size
XFISTX   ST    R0,SLXLEN(,R7)              xlen = L
         L     R10,SLXLEN(,R7)             R10 = remaining = L
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
         LA    R4,SLSTAGE(,R7)             R4 = &stage (dst B1)
         ALR   R4,R11
         LA    R3,MVCKK8                   R3 = source key 8
         DC    X'D9134000',X'5000'         MVCK 0(1,4),0(5),3
         ALR   R11,R0                      off += piece
         SLR   R10,R0                      remaining -= piece
         B     WRINLP
WRINEND  DS    0H
         XC    SLRECB(4,R7),SLRECB(R7)     reply_ecb = 0
         LA    R3,FNXFER
         ST    R3,SLXFUNC(,R7)             xfunc = XFER
         LA    R3,STPEND
         ST    R3,SLSTATE(,R7)             publish PENDING
         B     DOPOST                      EXPLICIT: see the note below
*----------------------------------------------------------------------
*  Every staging block above ends with an EXPLICIT B DOPOST, including
*  the
*  one immediately ahead of DOPOST.  Before Stage-0c the XFER block
*  simply
*  fell through into the POST block, and inserting ORPHAN between the
*  two
*  silently redirected XFER into it -- staging the ORPHAN identity
*  (zeros
*  for an XFER caller), which the guard then correctly called UNKNOWN
*  and
*  the client waited forever for a reply that must not be sent.  The
*  assembler cannot see that, a green link cannot see it, and neither
*  can
*  a probe that never issues an XFER: it took a live TSTUBUF hang.  Do
*  not
*  "clean up" these branches.
*----------------------------------------------------------------------
*  RQE write-in (M5-2a, ADR-0041).  Carries a real request across: the
*  user
*  data AND the 64-byte NSFRQE that describes it.  Two moves, in this
*  order:
*
*    1. ubuf -> stage[], clamped L = min(ulen, XFCHUNK), exactly as
*    XFER.
*       The clamp is what the dispatcher is later told through the
*       NSFRQE's
*       ulen, so the op reports the count that actually crossed
*       (ADR-0041 2).
*    2. the caller's NSFRQE image -> ANCRQE, one 64-byte MVCK piece (64
*    <=
*       MVCKMAX, so no loop is needed).
*
*  Both are source key 8 -> dst key 0, raw D9: as370 mis-assembles the
*  MVCK
*  mnemonic (drops the R1/R3 registers), see tstmvck.c.  Piece length
*  rides
*  in R0 because MVCK is not trusted to preserve R1.
*----------------------------------------------------------------------
RQEIN    DS    0H
         L     R0,REQULEN(,R8)             R0 = ulen
         C     R0,=A(XFCHUNK)              > staging size?
         BNH   RQISTX
         L     R0,=A(XFCHUNK)              clamp to staging size
RQISTX   ST    R0,SLXLEN(,R7)              xlen = L (kept across WAIT)
         L     R10,SLXLEN(,R7)             R10 = remaining = L
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
         LA    R4,SLSTAGE(,R7)             R4 = &stage (dst B1)
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
         LA    R4,SLRQE(,R7)               R4 = &slot.rqe (dst B1)
         LA    R1,RQELEN                   R1 = 64 (one piece)
         LA    R3,MVCKK8                   R3 = source key 8
         DC    X'D9134000',X'5000'         MVCK 0(1,4),0(5),3
RQINPUB  DS    0H
         XC    SLRECB(4,R7),SLRECB(R7)     reply_ecb = 0
         LA    R3,FNRQE
         ST    R3,SLXFUNC(,R7)             xfunc = RQE
         LA    R3,STPEND
         ST    R3,SLSTATE(,R7)             publish PENDING
         B     DOPOST                      EXPLICIT: never fall through
*----------------------------------------------------------------------
*  Wake the STC: cross-AS branch POST via CVT0PT01, supervisor key 0
*  (the
*  exact @@xmpost.c sequence).  Only R9 survives the POST, so preserve
*  our
*  registers with STM/LM into the anchor's 18-word save area; R9
*  carries the
*  save-area pointer.  R2 (anchor), R8 (A(req)), R14 (return) all
*  restore
*  from the save area afterward, and the later WAIT (SVC 1) preserves
*  R2-R14.
*----------------------------------------------------------------------
DOPOST   DS    0H                 POST/WAIT entry (all verbs share)
*  PER-INVOCATION save area (M5-2b2): the SVRB's own extended save
*  area,
*  which the FLIH allocates per SVC invocation, so two clients in two
*  address
*  spaces get two areas with no lock and no pool.  See the RBEXSAVE
*  block
*  above -- and note that A(SVRB) comes from TCBRBP, NOT from R5, which
*  is
*  the MVCK source pointer in RQEIN and holds A(caller image) by now.
*
*  FIVE registers now, not four: R7 (the SLOT BASE, M5-2b3) joins R2,
*  R6 and
*  R8 plus R14.  Nothing after the POST can re-derive which slot is
*  ours --
*  the claim is long past and the scan would find a different free one
*  -- so
*  R7 is as load-bearing across the POST as the anchor base itself.
*
*  b2's five-word self-check is GONE with ANCSAVE (issue #61).  It was
*  one-time evidence that this home is real; it has been collected, it
*  is
*  recorded in ADR-0038, and TSTRQXF (C) is converted into the pool's
*  own
*  positive check rather than deleted -- same shape of evidence, aimed
*  at
*  what is now unproven instead of what is now settled.
         SLR   R9,R9
         L     R9,PSATOLD(,R9)    A(current TCB)
         L     R9,TCBRBP(,R9)     A(current RB) = our SVRB
         LA    R9,RBEXSAVE(,R9)   R9 -> SVRB extended save area
         ST    R14,0(,R9)         the five live across the POST
         ST    R2,4(,R9)
         ST    R6,8(,R9)
         ST    R8,12(,R9)
         ST    R7,16(,R9)         the slot base -- not re-derivable
         L     R10,=X'40000000'   POST completion code (0)
*  POST target: the STC's published key-8 private ECB if it has one,
*  else
*  the key-0 CSA server_ecb.  The executive WAITs from problem state,
*  where a
*  key-0 ECB is a documented abend (S047 / X'201'); the Stage-0 probe
*  STC
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
         L     R14,0(,R9)         restore the five
         L     R2,4(,R9)
         L     R6,8(,R9)
         L     R8,12(,R9)
         L     R7,16(,R9)         the slot base
         B     PSTOK
PSTERR   DS    0H                 POST failed: STC ASCB gone
         L     R14,0(,R9)
         L     R2,4(,R9)
         L     R6,8(,R9)
         L     R8,12(,R9)
         L     R7,16(,R9)         the slot base
         B     PSTFAIL
PSTOK    DS    0H
*  ORPHAN leaves here: the STC is awake, the request is in flight, and
*  this
*  caller neither waits for the reply nor gives the in-flight count
*  back.
*----------------------------------------------------------------------
*  WAIT for the reply on the key-0 CSA reply ECB, supervisor state, key
*  0.
*  ADR-0038 empirical unknown #1: Stage-0a's problem-state /
*  key-8-stack-ECB
*  rule does NOT transfer -- the routine never leaves key 0.  On wake
*  the STC
*  has either serviced us (state -> DONE) or, on quiesce, posted us to
*  bail.
*----------------------------------------------------------------------
         LA    R3,SLRECB(,R7)     A(our slot's reply_ecb)
         WAIT  1,ECB=(R3)
         CLC   ANCEYE(8,R2),=CL8'NSFVANCR'   anchor there?
         BNE   WGONE              freed while parked -> bail
         L     R3,SLSTATE(,R7)
         C     R3,=A(STDONE)      serviced?
         BNE   WQUIES             no -> quiesce wake, bail
*----------------------------------------------------------------------
*  Normal reply: copy the STC's echo + served into the caller's block,
*  release
*  the slot, decrement in-flight LAST (after every CSA write).
*----------------------------------------------------------------------
         L     R3,REQFUNC(,R8)    request function
         C     R3,=A(FNXFER)      XFER?
         BE    XFEROUT
         C     R3,=A(FNRQE)       RQE (M5-2a)?
         BE    RQEOUT
*  ECHO: copy the echoed token (token+1) back into the caller's block.
         L     R3,SLTOKEN(,R7)    echoed token
         ST    R3,REQTOKN(,R8)
         B     REPLYC
*----------------------------------------------------------------------
*  XFER read-out: move the transformed staging back out to the caller's
*  ubuf,
*  L = the staged xlen (reloaded from CSA -- it survived POST/WAIT), <=
*  MVCMAX
*  bytes per piece, offset recomputed.  The TRUE piece length is kept
*  in R0 to
*  advance the loop, because EX wants R1 = length-1 and R1 is
*  decremented in
*  place.  The SOURCE was never the hazard: the staging buffer IS key-0
*  CSA and
*  reading it is correct.
*
*  THE DESTINATION-KEY WINDOW APPLIES HERE TOO (M5-2c0, ADR-0039).  The
*  move
*  runs through MOVEOUT, exactly as RQEOUT's does, so the
*  caller-supplied
*  DESTINATION is checked by the hardware against the CALLER's key
*  instead of
*  being stored under this routine's own key 0.
*
*  IT DID NOT UNTIL M5-2c0, and the reason it does now is worth
*  recording.
*  M5-2b1 closed the window on RQEOUT -- the M5-2 transport -- and left
*  XFER
*  alone on the recorded assumption that M5-2c would DELETE the verb. 
*  That
*  assumption does not hold: removing XFER retires TSTUBUF, the only
*  gate that
*  proves the keyed ubuf bounce, so the verb survives to c3 at the
*  earliest.
*  Until then FNXFER is dispatchable by an UNAUTHORISED client
*  (test/mvs/
*  tstubuf.c drives it today), which makes a key-0 store into a
*  caller-supplied
*  address a LIVE path rather than scaffolding.
*
*  A plain MVC through MOVEOUT, not MVCK: the reasoning is MOVEOUT's
*  own
*  header and is deliberately not restated here.
*----------------------------------------------------------------------
XFEROUT  DS    0H
*  Window setup, ONCE -- an inline COPY of the nine instructions at the
*  top of
*  RQEOUT, deliberately byte-identical to them.  RQEOUT is the
*  countersigned
*  production write-out path and is left untouched here, so the two
*  setups can
*  be diffed and seen to be the same; extracting them into a shared
*  subroutine
*  would move instructions in that path and drag its whole live round
*  back in.
*  THE COPY EXPIRES WITH THE VERB (c3).  What R9 and R12 come out
*  holding, and
*  why neither reads as a small integer, is explained ONCE at RQEOUT --
*  that
*  block is the explanation of record.  R3 is free here: it holds the
*  REQFUNC
*  comparand the dispatch above tested and nothing reads it again.
         LR    R9,R2              park the anchor base across IPK
         IPK   0                  R2 = PSW key in bits 24-27
         LR    R12,R2             R12 = SPKA operand (restore)
         LR    R2,R9              anchor base back
         SLR   R9,R9
         L     R9,PSATOLD(,R9)    R9 = A(caller TCB)
         SLR   R3,R3
         IC    R3,TCBPKF(,R9)     caller's key, high nibble
         LR    R9,R3              R9 = SPKA operand (borrow)
         L     R10,SLXLEN(,R7)    R10 = remaining = L
         SLR   R11,R11            R11 = offset
RDOTLP   LTR   R10,R10            bytes left? (0 -> skip)
         BNP   REPLYC
         LR    R1,R10             R1 = piece length
         C     R1,=A(MVCMAX)
         BNH   RDOTSZ
         LA    R1,MVCMAX          cap at 256
RDOTSZ   LR    R0,R1              save TRUE piece length
         L     R4,REQUBUF(,R8)    R4 = ubuf base (dst B1)
         ALR   R4,R11
         LA    R5,SLSTAGE(,R7)    R5 = &stage (source B2)
         ALR   R5,R11
         BCTR  R1,0               EX takes LENGTH-1
         BAL   R15,MOVEOUT        move under the caller's key
         ALR   R11,R0             off += piece
         SLR   R10,R0             remaining -= piece
         B     RDOTLP
REPLYC   DS    0H
         L     R3,ANCSERVD(,R2)
         ST    R3,REQSEQ(,R8)
         SLR   R3,R3
         ST    R3,REQRC(,R8)      caller rc = OK
*  RELEASE BY A PLAIN ST, NOT BY CS (ADR-0042 2).  The owner releasing
*  its
*  own slot races with nobody: no other client can claim a slot that is
*  not
*  FREE, and we are the only party that turns it FREE.  S/370 stores
*  are
*  ordered, so this is simply the last store after everything the
*  client
*  will read -- which is why it comes AFTER the read-out above.
         LA    R3,STFREE
         ST    R3,SLSTATE(,R7)    DONE -> FREE (this slot only)
         L     R3,ANCINFL(,R2)
UDEC1    LR    R4,R3
         BCTR  R4,0
         CS    R3,R4,ANCINFL(R2)
         BNE   UDEC1
         SLR   R15,R15            R15 = RCOK
         BR    R14
*----------------------------------------------------------------------
*  RQE read-out (M5-2a, ADR-0041).  The mirror of RQEIN, in the same
*  order:
*  the transformed staging back out to the caller's ubuf
*  (read-direction
*  data), then the 64-byte NSFRQE image back to the caller's block.
*
*  The image is copied WHOLE, but only the STC's result fields differ:
*  the
*  STC's copy-out writes retcode/errno_/apptok/p1/p2/p3 into the slot
*  and
*  nothing else (ADR-0041 4), so every caller-owned field still holds
*  the
*  value the caller sent.  Which fields are actually APPLIED to the
*  caller's
*  live NSFRQE is decided one level up, in C, by nsfreqx_result_in --
*  that is
*  where the field policy is host-tested, and it is deliberately not
*  duplicated here in assembler.
*
*  THE DESTINATION-KEY WINDOW (M5-2b1, ADR-0039).  Both moves below
*  store into
*  CALLER-SUPPLIED addresses -- ubuf, and since M5-2a the 64-byte
*  NSFRQE image
*  -- so under the routine's own PSW key 0 the hardware would NOT check
*  either
*  against the caller's key, and a wrong or hostile pointer would be a
*  silent
*  clobber.  Both therefore run inside a NARROW SPKA window set to the
*  CALLER's key: see MOVEOUT below, which is the only code that leaves
*  key 0.
*
*  The SOURCE key was never the hazard -- the staging buffer and the
*  slot ARE
*  key-0 CSA and reading them is correct.  Do not conflate the two
*  halves.
*----------------------------------------------------------------------
RQEOUT   DS    0H
*  Window setup, ONCE.  R9 and R12 come out holding SPKA OPERANDS, NOT
*  keys.
*  SPKA takes the key from bits 24-27 of its operand ADDRESS and
*  ignores
*  every other bit, so neither register reads as a small integer in a
*  dump:
*
*    R9  = the TCBPKF byte itself, key in the HIGH nibble --
*    X'00000080'
*          for a key-8 caller.  80, not 08.
*    R12 = the anchor base with IPK's result merged into its low byte. 
*    IPK
*          writes ONLY bits 24-27 (and zeroes 28-31); bits 0-23 keep
*          what
*          they had, which here is the top of the anchor address.  For
*          an
*          anchor at X'00A6F688' under key 0 that reads X'00A6F600'.
*
*  THAT IS NOT CORRUPTION.  R12 is never used as an address and never
*  dereferenced -- only SPKA reads it, and only four bits of it. 
*  Restoring
*  through the value IPK produced is deliberate: it puts back whatever
*  key
*  the routine actually ran under instead of assuming 0.
*
*  IPK writes R2 -- our anchor base -- so R2 is parked across it and
*  put
*  straight back.  R9 and R12 are untouched by the loop and by MOVEOUT.
         LR    R9,R2              park the anchor base across IPK
         IPK   0                  R2 = PSW key in bits 24-27
         LR    R12,R2             R12 = SPKA operand (restore)
         LR    R2,R9              anchor base back
         SLR   R9,R9
         L     R9,PSATOLD(,R9)    R9 = A(caller TCB)
         SLR   R3,R3
         IC    R3,TCBPKF(,R9)     caller's key, high nibble
         LR    R9,R3              R9 = SPKA operand (borrow)
         L     R10,SLXLEN(,R7)    R10 = remaining = L
         SLR   R11,R11            R11 = offset
RQOTLP   LTR   R10,R10            bytes left? (0 -> skip)
         BNP   RQOTRQE
         LR    R1,R10             R1 = piece length
         C     R1,=A(MVCMAX)
         BNH   RQOTSZ
         LA    R1,MVCMAX          cap at 256
RQOTSZ   LR    R0,R1              save TRUE piece length
         L     R4,REQUBUF(,R8)    R4 = ubuf base (dst B1)
         ALR   R4,R11
         LA    R5,SLSTAGE(,R7)    R5 = &stage (source B2)
         ALR   R5,R11
         BCTR  R1,0               EX takes LENGTH-1
         BAL   R15,MOVEOUT        move under the caller's key
         ALR   R11,R0             off += piece
         SLR   R10,R0             remaining -= piece
         B     RQOTLP
RQOTRQE  DS    0H
         L     R4,REQRQEI(,R8)    R4 = A(caller NSFRQE) dst B1
         LTR   R4,R4              no image supplied?
         BZ    REPLYC             then nothing to give back
         LA    R5,SLRQE(,R7)      R5 = &slot.rqe (source B2)
         LA    R1,RQELEN-1        R1 = 63 (EX length-1)
         BAL   R15,MOVEOUT        move under the caller's key
         B     REPLYC             EXPLICIT: never fall through
*----------------------------------------------------------------------
*  MOVEOUT -- the write-out key window (M5-2b1, ADR-0039).  Moves R1+1
*  bytes
*  from 0(R5) to 0(R4) with the PSW key set to the CALLER's, so the
*  hardware
*  checks the caller-supplied DESTINATION against the key that owns it.
*   In:
*  R1 = length-1, R4 = dst, R5 = src, R9/R12 = SPKA OPERANDS (bits
*  24-27 are
*  the borrowed and the restored key respectively -- see the RQEOUT
*  header;
*  they are not key integers and R12 is not an address).  Link R15.
*
*  THIS IS THE ONLY BLOCK IN THE ROUTINE THAT RUNS UNDER A KEY OTHER
*  THAN 0,
*  AND IT MUST STAY THAT WAY.  Four instructions, THREE call sites:
*  that is
*  what makes "what executes under a borrowed key" a question with a
*  short,
*  checkable answer, and the property is easy to lose and hard to get
*  back.
*  M5-2c0 added the third (XFEROUT's read-out) -- a CALLER, not a
*  second
*  such block, which is the only way this stays one question.
*  The window is per piece rather than around the loop for the same
*  reason --
*  the routine's own bookkeeping never executes under a key it does not
*  control, and the ONLY instruction that can take a protection
*  exception is
*  the move itself.
*
*  b3 WILL BE TEMPTED to wrap the slot-pool CS claim loop in a window
*  too.
*  Do not: a claim needs an interlocked compare (CS), which is a
*  serialization problem, not a key problem.  The two are unrelated and
*  conflating them would put a retry loop under a borrowed key for no
*  gain.
*
*  A plain MVC, not MVCK: b0 (tstmvcd.c) measured that under the
*  caller's key
*  BOTH operands are reachable -- the key-0 CSA staging is not
*  fetch-protected
*  (ISK X'06') so it reads, and the caller's private storage is its own
*  key so
*  it writes.  MVCK inside a non-zero PSW key would put its R3 source
*  key
*  through the CR3 key-mask check that already cost tstmvck.c an S0C2;
*  buying
*  that unknown back to reuse an encoding would be the wrong trade. 
*  MVC takes
*  its length from the instruction, hence EX -- which ORs R1's low byte
*  into a
*  COPY and modifies no storage, so the routine stays RENT (the same
*  trick
*  src/nsfreqc.c uses for the SVC number).
*
*  IF TCBPKF READS 0 the window is a no-op and there is no protection. 
*  That
*  is CORRECT, not a bug: a key-0 caller can already store anywhere, so
*  there
*  is nothing to protect it from.  Do not "fix" this into a hardcoded
*  key 8.
*----------------------------------------------------------------------
MOVEOUT  DS    0H
         SPKA  0(R9)              -> the caller's key
         EX    R1,MVCPIEC         move R1+1 bytes
         SPKA  0(R12)             -> back to our own key
         BR    R15
MVCPIEC  MVC   0(1,R4),0(R5)      EX target: length from R1
*----------------------------------------------------------------------
*  SLOTADR -- resolve a caller-supplied slot INDEX to a slot address.
*
*  In:  R3 = index, R2 = anchor.  Out: R10 = A(slot).  Clobbers R3, R4.
*  Link R15.  On an out-of-range index it does NOT return: it branches
*  straight to SLOTBAD, so no caller can accidentally continue with an
*  address it never computed.
*
*  RANGE-CHECKED AGAINST THE ANCHOR'S OWN nslots, the same bound the
*  claim
*  scan uses.  An unchecked index here would compute an address outside
*  the
*  allocation and STORE through it -- the IPL-class overrun ADR-0039 3
*  names,
*  reached from a probe verb rather than from a move.
*
*  The index is walked, not multiplied: at most 64 LA/BCT iterations on
*  a
*  probe path, and the routine then contains no multiply at all, which
*  keeps
*  "the scan performs no index multiply" (ADR-0042 4) literally true
*  rather
*  than true-of-the-scan-only.
*----------------------------------------------------------------------
SLOTADR  DS    0H
         L     R4,ANCNSLOT(,R2)   the allocator's count
         CLR   R3,R4              index < nslots?
         BNL   SLOTBAD            no -> reject, do not compute
         LA    R10,ANCSLOTS(,R2)  R10 = &slots[0]
         LTR   R3,R3              index 0?
         BZ    SLOTADX
SLOTALP  LA    R10,SLOTLEN(,R10)  walk one slot
         BCT   R3,SLOTALP
SLOTADX  BR    R15
*----------------------------------------------------------------------
*  Probe handlers (Stage-0c ADR-0040 8, extended by M5-2b3 ADR-0042).
*  PROBE-ONLY: none of this is part of the M5-2 transport, and all of
*  it is
*  due out in M5-2c -- which is a SECURITY item, not hygiene, because
*  these
*  verbs are reachable by an unauthorised client.
*
*  ORPHAN was retired in M5-2c2 stage b: it was the one place a
*  REQUEST-SUPPLIED identity was stored verbatim, which is exactly what
*  the guard must never trust from a real client.  Its code is now
*  rejected ahead of the claim (see FNORPH at the pre-claim chain).
*
*  DOQUERY reports ONE slot's state plus the
*  global counters to a client that cannot read CSA itself; DOUNSTG
*  releases
*  a named slot the STC declined to release; DOSLOT compare-and-swaps a
*  named
*  slot's state so a test can pre-claim.
*----------------------------------------------------------------------
*
*  QUERY: req.slot names the slot whose state to report; the counters
*  are
*  global.  Changes nothing, and works while the slot is busy -- that
*  is the
*  state the probe needs to observe.
DOQUERY  DS    0H
         L     R3,REQSLOT(,R8)    which slot?
         BAL   R15,SLOTADR        R10 = A(slot) or -> SLOTBAD
         L     R3,SLSTATE(,R10)
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
*  UNSTAGE: FREE a named slot and give one in-flight count back, but
*  never
*  take the count below zero -- the STC may already have reaped this
*  request.
DOUNSTG  DS    0H
         L     R3,REQSLOT(,R8)    which slot?
         BAL   R15,SLOTADR        R10 = A(slot) or -> SLOTBAD
         L     R3,SLSTATE(,R10)
         LTR   R3,R3              already FREE?
         BZ    UNSTRC             yes -> nothing to give back
         SLR   R3,R3
         ST    R3,SLSTATE(,R10)   -> FREE
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
*
*  SLOT (M5-2b3): CS one named slot's state from req.sexpect to
*  req.snew, so
*  a test can pre-claim slots and then observe that the scan SKIPS
*  exactly
*  those, or fill the pool and observe ENOBUFS.  An unauthorised client
*  cannot store into CSA itself; this routine (key 0) can.
*
*  IT IS A CS, NOT A BLIND STORE, for the same reason the reaper is: a
*  blind
*  "set slot i to CLAIMED" would stomp a LIVE claim if the test
*  miscounted,
*  and that failure would present as a pool bug.  A failed compare
*  answers
*  RCNOREQ, so the test asserts the pre-claim TOOK rather than
*  inferring it
*  from the absence of a complaint (CLAUDE.md 8.5).
*
*  Touches no in-flight count: it moves a slot between states, it does
*  not
*  make or finish a request.
DOSLOT   DS    0H
         L     R3,REQSLOT(,R8)    which slot?
         BAL   R15,SLOTADR        R10 = A(slot) or -> SLOTBAD
         L     R3,REQSEXP(,R8)    R3 = expected (comparand)
         L     R4,REQSNEW(,R8)    R4 = new state (swap-in)
         CS    R3,R4,SLSTATE(R10) took it?
         BNE   SLOTBSY            no -> NOREQ, and say so
         ST    R3,REQQSTA(,R8)    report the state we replaced
         SLR   R3,R3
         ST    R3,REQRC(,R8)      caller rc = OK
         SLR   R15,R15
         BR    R14
*----------------------------------------------------------------------
*  Bail paths.
*----------------------------------------------------------------------
WQUIES   DS    0H                 quiesce wake: give inflight back
         LA    R3,STFREE          so the shutdown drain completes
         ST    R3,SLSTATE(,R7)
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
         ST    R3,SLSTATE(,R7)
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
*  Retired verb (M5-2c2 stage b).  The pointer is trusted here -- the
*  eyecatcher was checked -- so the rc goes into the CALLER'S BLOCK and
*  not only R15.  BADREQ would leave req.rc at whatever the client
*  initialised it to, which is indistinguishable from "the SVC never
*  ran" (CLAUDE.md 8.5).  A client must be able to SEE the rejection.
BADFUNC  DS    0H                 retired verb: write rc
         LA    R15,RCINVAL
         ST    R15,REQRC(,R8)
         BR    R14
*
SLOTBSY  DS    0H                 named slot busy: write rc
         LA    R15,RCNOREQ
         ST    R15,REQRC(,R8)
         BR    R14
*
*  Pool exhausted (M5-2b3).  Every slot taken, or an anchor claiming
*  zero
*  slots.  ENOBUFS and RETURN -- no spinning and no WAIT: this runs in
*  SUPERVISOR STATE, KEY 0, INSIDE A CLIENT'S ADDRESS SPACE, on that
*  client's
*  TCB.  Spinning there burns a dispatchable unit at key 0 waiting on
*  an
*  unrelated address space; waiting there parks a task holding an SVRB
*  on
*  something nothing reliably posts.  A full pool is a HEALTHY stack
*  with no
*  slot right now, which is what nsfreqx_rc_errno turns into
*  NSF_ENOBUFS.
*
*  Reached BEFORE the in-flight increment, so a caller that got no slot
*  leaves inflight untouched -- which is what the live exhaustion check
*  asserts.
POOLFUL  DS    0H
         L     R3,ANCEXH(,R2)     diagnostic: how often
UEXHLP   LR    R4,R3
         LA    R4,1(,R4)
         CS    R3,R4,ANCEXH(R2)
         BNE   UEXHLP
         LA    R15,RCNOBUF
         ST    R15,REQRC(,R8)
         BR    R14
*
*  A probe verb named a slot index at or past nslots.  Reject WITHOUT
*  computing an address: an unchecked index would store outside the
*  allocation.
SLOTBAD  DS    0H
         LA    R15,RCINVAL
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
