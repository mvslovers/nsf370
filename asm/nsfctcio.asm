         TITLE 'NSFCTCIO - CTCI TOP HALF: C-CALLABLE EXCP PRIMITIVES'
*
*  NSFCTCIO - the HLASM top half of the CTCI driver src/nsfctci.c
*            (Spec 9.3; ADR-0019).  The file is named nsfctcio (not
*            nsfctci) because mbt derives an object name from the
*            source BASENAME, so asm/nsfctci.asm would collide with
*            src/nsfctci.c at build/nsfctci.o -- basenames must be
*            unique.  The CSECT entries keep the NSFCI* alias scheme.
*
*            It moves a RAW block of bytes across one 3088
*            subchannel with plain EXCP: no CTCIHDR/CTCISEG framing
*            (the C bottom half owns that, M1-4) and -- per ADR-0019 --
*            NO I/O-completion exit and NO CHE appendage.  EXCP starts
*            the channel program and returns; IOS POSTs the IOB ECB
*            (which IS the device ECB already in the Spec 5.3 ECBLIST)
*            when the channel program terminates.  The DCB is opened
*            WITHOUT CENDA=, so no appendage is ever activated.
*
*            The module holds NO per-device state: it keeps ONE
*            read-only MODEL DCB and copies it into a per-subchannel
*            control block (CTCISC = DCB copy + IOB + CCW) that the
*            CALLER supplies from the NSF region (NETDEV.priv, via
*            src/nsfctci.c).  N devices, 2 subchannels each, thus work
*            from one CSECT.  Modeled on mvslovers/mvs38j-ip mvsctc.asm
*            -- the MVS EXCP convention is taken, its Xinu scheduler
*            wiring is NOT (ADR-0019).
*
*  C-CALLABLE ENTRIES.  Args are a fullword list at R1; a value result
*  returns in R15.  scb = a CTCISC-sized block; ecb = a caller-owned
*  fullword ECB.
*    ctci_scb_size()                 -> sizeof(CTCISC), for mm_alloc
*    ctci_open_sub(scb,forwrite,ddn) -> copy model, patch DDNAME, OPEN
*                                       INPUT/OUTPUT; rc 0 = opened
*    ctci_read (scb,buf,len,ecb)     -> READ  CCW X'02'+SLI, EXCP
*    ctci_write(scb,buf,len,ecb)     -> WRITE CCW X'01', EXCP
*    ctci_status(scb,postptr,resptr) -> *post = IOBECBCC post code,
*                                       *res  = CSW residual count
*    ctci_close_sub(scb)             -> CLOSE the subchannel
*
*  ENTRY CONVENTION (issue #8; CLAUDE.md 3): every entry is built the
*  STANDARD cc370 way -- COPY MVSMACS + COPY PDPTOP, FUNHEAD / FUNEXIT,
*  modeled on libc370 @@getclk.asm.  A hand-rolled STM/BALR/USING seam
*  breaks the cc370 C-runtime path (@@CRTGET, ABEND S0C6).  The four
*  macro-issuing entries (OPEN/CLOSE/EXCP are SVCs) use FUNHEAD SAVE=
*  so a called service has a save area; the two leaf entries
*  (scb_size, status) issue no SVC and use the plain leaf form (like
*  nsf_now).  The FUNHEAD entry name IS the 8-char asm() alias in
*  include/nsfctci.h, character for character (PR #7): NSFCISZ /
*  NSFCIOPN / NSFCIRD / NSFCIWR / NSFCIST / NSFCICL.
*
*  AS370 QUIRKS kept in force (each cost a live ABEND in an M0 seam):
*   1) Address static data (the MODEL DCB) by EXPLICIT displacement
*      LABEL-entry(,R12), never a bare-label USING -- as370 drops those
*      to base 0 (the S102 class, ADR-0011 seam bug).
*   2) Keep every statement inside column 71 -- as370 reads column 72
*      as a continuation flag and silently merges the next line.
*   3) as370 (cc370 #18) assembles a bare SYM(Rn) for a relocatable
*      DSECT field with disp = SYM - (active USING base address),
*      instead of disp = SYM.  FUNHEAD sets USING entry,R12 and every
*      entry after the first is at a non-zero CSECT offset, so a bare
*      DCBDDNAM(R2) would point that many bytes low.  A DIFFERENCE
*      expression is immune (the two DSECT symbols cancel to an
*      absolute), so EVERY DCB/IOB field is addressed FIELD-<origin>:
*      DCB fields  DCBDDNAM-IHADCB(R2)  (R2 = DCB start; IHADCB origin)
*      IOB fields  IOBECBPB-IOBSTDRD(R7)  (R7 = standard IOB, what EXCP
*      wants; the IEZIOB symbols are offsets from the 16-byte-earlier
*      prefix origin, so -IOBSTDRD also rebases them onto it).
*      (The IHADCB / IEZIOB DSECTs are declared before the code purely
*      for tidiness; it is the difference form that fixes the bug.)
*   4) CCW / IOB doubleword alignment: the CAW requires the first CCW
*      on a doubleword boundary.  The caller's scb is an mm_alloc
*      payload, doubleword-aligned on MVS (region from malloc, objsize
*      rounded to 8, MMOBJ = 8 -- see nsfmm.c), and CTCISC places SCIOB
*      and SCCCW on DS 0D boundaries, so both are aligned.
*
*  ==============  EXCP path VALIDATED on MVS (issue #16)  =========
*  Run live on MVSCE against a real Hercules 3088 CTCI pair (CUU
*  500/501 on tun0).  OPEN both subchannels, one raw EXCP each way:
*  WRITE post X'7F' 38/38 bytes (crafted ICMP seen in host tcpdump),
*  READ post X'7F', length = requested - IOB residual.  The S102/S0C6/#8
*  unvalidated-seam risk is retired for this module.  (The READ block
*  DECODE -- walking CTCISEGs to the leading hwOffset -- is M1-4, and
*  §9.3's old "chain to a 0x0000 terminator" model was wrong: see the
*  corrected §9.3 / ADR-0020.)
*  =================================================================
*
         COPY  MVSMACS
         COPY  PDPTOP
*  DSECTs before the code (tidiness; difference form is the fix, quirk 3).
         DCBD  DSORG=PS
         IEZIOB ,
         CSECT ,
         PRINT GEN
*
CIREAD   EQU   X'02'              READ  CCW opcode (host->guest)
CIWRITE  EQU   X'01'              WRITE CCW opcode (guest->host)
CISLI    EQU   X'20'              suppress-incorrect-length CCW flag
*  IOBUNREL (X'02') comes from IEZIOB above -- do not redefine it.
*---------------------------------------------------------------------*
*  ctci_scb_size() -> UINT                                            *
*     Bytes the caller must mm_alloc per subchannel block (CTCISC).   *
*---------------------------------------------------------------------*
NSFCISZ  FUNHEAD ,                return sizeof(CTCISC)
         LA    R15,CTCISCL        R15 = CTCISC length (absolute EQU)
         FUNEXIT RC=(R15)
*---------------------------------------------------------------------*
*  ctci_open_sub(scb, forwrite, ddname8) -> INT                       *
*     Copy the MODEL DCB into scb, patch the returned DDNAME, OPEN    *
*     INPUT (forwrite==0) or OUTPUT (forwrite!=0).  rc 0 = opened.    *
*---------------------------------------------------------------------*
NSFCIOPN FUNHEAD SAVE=CTCISAVE,US=NO
         L     R2,0(,R1)          R2 = scb (DCB copy sits at scb+0)
         L     R6,4(,R1)          R6 = forwrite (0=read, else write)
         L     R3,8(,R1)          R3 = ddname (8 chars)
         LA    R8,MODLDCB-NSFCIOPN(,R12)   R8 = &model DCB
         MVC   0(SCDCBL,R2),0(R8)          scb DCB = model DCB
         MVC   DCBDDNAM-IHADCB(8,R2),0(R3)  copied DCB ddname = *ddname
         LTR   R6,R6              write subchannel?
         BNZ   CIOPENW
         OPEN  ((R2),INPUT)       read subchannel
         B     CIOPCHK
CIOPENW  OPEN  ((R2),OUTPUT)      write subchannel
CIOPCHK  TM    DCBOFLGS-IHADCB(R2),DCBOFOPN   opened OK?
         BNO   CIOPBAD
         SLR   R15,R15            rc = 0 (opened)
         FUNEXIT RC=(R15)
CIOPBAD  LA    R15,1              rc = 1 (open failed)
         FUNEXIT RC=(R15)
*---------------------------------------------------------------------*
*  ctci_read(scb, buf, len, ecb) -> INT   EXCP an inbound READ.       *
*---------------------------------------------------------------------*
NSFCIRD  FUNHEAD SAVE=CTCISAVE,US=NO
         L     R2,0(,R1)          R2 = scb
         L     R4,4(,R1)          R4 = buffer
         L     R5,8(,R1)          R5 = length
         L     R3,12(,R1)         R3 = &ecb
         LA    R6,SCCCWO(,R2)     R6 = &CCW in scb
         MVI   0(R6),CIREAD       opcode = unchained READ
         STCM  R4,B'0111',1(R6)   CCW data address = buffer
         MVI   4(R6),CISLI        flags = SLI
         MVI   5(R6),X'00'        reserved
         STCM  R5,B'0011',6(R6)   CCW count = length
         BAL   R14,CISTART        clear ECB, build IOB, EXCP
         SLR   R15,R15            rc = 0 (started)
         FUNEXIT RC=(R15)
*---------------------------------------------------------------------*
*  ctci_write(scb, buf, len, ecb) -> INT   EXCP an outbound WRITE.    *
*---------------------------------------------------------------------*
NSFCIWR  FUNHEAD SAVE=CTCISAVE,US=NO
         L     R2,0(,R1)          R2 = scb
         L     R4,4(,R1)          R4 = buffer
         L     R5,8(,R1)          R5 = length
         L     R3,12(,R1)         R3 = &ecb
         LA    R6,SCCCWO(,R2)     R6 = &CCW in scb
         MVI   0(R6),CIWRITE      opcode = unchained WRITE
         STCM  R4,B'0111',1(R6)   CCW data address = buffer
         MVI   4(R6),X'00'        flags = none
         MVI   5(R6),X'00'        reserved
         STCM  R5,B'0011',6(R6)   CCW count = length
         BAL   R14,CISTART        clear ECB, build IOB, EXCP
         SLR   R15,R15            rc = 0 (started)
         FUNEXIT RC=(R15)
*---------------------------------------------------------------------*
*  CISTART (internal, BAL R14)   finish an I/O request:               *
*     clear the ECB, clear + fill the IOB, EXCP.  On entry:           *
*       R2 = scb   R3 = &ecb   R6 = &CCW (already built)              *
*     Clearing the ECB FIRST is mandatory (ADR-0019): a stale posted  *
*     bit makes the loop process a phantom completion.                *
*---------------------------------------------------------------------*
CISTART  DS    0H
         XC    0(4,R3),0(R3)      clear the caller's ECB
         LA    R7,SCIOBO(,R2)     R7 = &IOB (standard section)
         XC    0(SCIOBL,R7),0(R7)  clear the IOB
         MVI   IOBFLAG1-IOBSTDRD(R7),IOBUNREL   unrelated request
         STCM  R3,B'0111',IOBECBPB-IOBSTDRD(R7)  IOB -> ECB
         STCM  R6,B'0111',IOBSTRTB-IOBSTDRD(R7)  IOB -> chan program
         LR    R0,R2              R0 = &DCB (scb+0)
         STCM  R0,B'0111',IOBDCBPB-IOBSTDRD(R7)  IOB -> DCB
         EXCP  (R7)               start the channel program (async)
         BR    R14
*---------------------------------------------------------------------*
*  ctci_status(scb, postptr, resptr)   decode a completed request.    *
*     *postptr = IOBECBCC (post code; X'7F' = normal completion).     *
*     *resptr  = CSW residual count.  The C bottom half computes      *
*     length = requested - residual.  Leaf: reads the IOB only, so it *
*     issues no SVC and needs no save area.                           *
*---------------------------------------------------------------------*
NSFCIST  FUNHEAD ,                read completion status
         L     R2,0(,R1)          R2 = scb
         L     R3,4(,R1)          R3 = postptr
         L     R4,8(,R1)          R4 = resptr
         LA    R7,SCIOBO(,R2)     R7 = &IOB (standard section)
         SLR   R0,R0
         IC    R0,IOBECBCC-IOBSTDRD(R7)   R0 = post code (IOB+4)
         ST    R0,0(,R3)          *postptr = post code
         SLR   R0,R0
         ICM   R0,B'0011',IOBUSTAT+2-IOBSTDRD(R7)  residual (+14)
         ST    R0,0(,R4)          *resptr = residual count
         FUNEXIT
*---------------------------------------------------------------------*
*  ctci_close_sub(scb) -> INT   CLOSE the subchannel (direction       *
*     agnostic).  The C layer unallocates the CUU (SVC 99) after.     *
*---------------------------------------------------------------------*
NSFCICL  FUNHEAD SAVE=CTCISAVE,US=NO
         L     R2,0(,R1)          R2 = scb (DCB at scb+0)
         CLOSE ((R2),)
         SLR   R15,R15            rc = 0
         FUNEXIT RC=(R15)
*
         LTORG ,
*---------------------------------------------------------------------*
*  Read-only MODEL DCB (copied per subchannel) + shared save area.    *
*  DSORG=PS,MACRF=E (EXCP); DDNAME is a placeholder patched at open.  *
*  CENDA= is DELIBERATELY OMITTED -- naming an appendage would        *
*  activate one (ADR-0019).  IOBAD names a dummy model IOB the macro  *
*  wants; the per-request IOB is the scb's SCIOB, passed to EXCP.     *
*---------------------------------------------------------------------*
         DS    0D
MODLDCB  DCB   DSORG=PS,MACRF=E,DDNAME=NSFCTCID,IOBAD=MODLIOB
MODLDCBE DS    0X
         DS    0D
MODLIOB  DC    XL64'00'           dummy model IOB for the DCB macro
*  One shared static save area: only the single executive task calls
*  these entries, run-to-completion, never nested -- so one 18F area
*  is safe (the single-task rationale of nsfstim's static state).
*  CONSTRAINT (issue #16 item 3): this ALSO assumes ESTAE never RETURNS
*  into an interrupted NSFCI* entry.  Benign today -- nsf_recover
*  percolates and never retries, so the recovery path (-> ctci_close ->
*  NSFCICL) reuses CTCISAVE but nothing resumes the interrupted NSFCIRD/
*  NSFCIWR whose caller regs it held.  If §17 ever adds RETRY, the
*  recovery entries need their own save area or this corrupts.
         DS    0D
CTCISAVE DC    18F'0'
*  Model-DCB length (the MVC copy size), a backward reference here.
SCDCBL   EQU   MODLDCBE-MODLDCB
*---------------------------------------------------------------------*
*  CTCISC -- the per-subchannel control block the caller supplies.    *
*    +0        the copied DCB       (SCDCBL bytes)                    *
*    SCIOBO    the IOB              (SCIOBL bytes, doubleword)        *
*    SCCCWO    the single CCW       (8 bytes, doubleword)             *
*  SCIOBO / SCCCWO / CTCISCL are absolute EQUs (immune to quirk 3).   *
*---------------------------------------------------------------------*
SCIOBL   EQU   64                 IOB standard section + IOS slop
CTCISC   DSECT
SCDCB    DS    CL(SCDCBL)         copied DCB (subchannel DDNAME)
         DS    0D
SCIOB    DS    CL(SCIOBL)         request IOB (doubleword aligned)
         DS    0D
SCCCW    DS    D                  single unchained CCW (doubleword)
CTCISCL  EQU   *-CTCISC           total bytes per subchannel block
SCIOBO   EQU   SCIOB-CTCISC       IOB offset within CTCISC
SCCCWO   EQU   SCCCW-CTCISC       CCW offset within CTCISC
         END
