//TSTD1BW JOB (A),'NSF 101 ARM3 WEDGE',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* #101 Stage 2, ARM 3 -- ROLE W (the parker).  TWO REAL ADDRESS SPACES.
//*
//* W parks a BLOCK-FOREVER cross-AS SELECT (no timeout at all) on its own
//* listener, through the facade.  A parked request holds the STC's single
//* private NSFRQE (ADR-0042 10), so cross-AS request service stalls for
//* everyone until it completes.  THAT MUCH IS TRUE ON BOTH MODULES.
//*
//* What the #101 fix changes is whether the wedge can ever LIFT.
//* nsfsel_on_notify re-scans the STORED item array, so with a count-valued
//* ulen those items are residue and no readiness poke can ever match them.
//*
//* ORDER:
//*   1. S NSFS, confirm it is up.
//*   2. Submit TSTD1BW.  Wait for the console line
//*        TSTD1B: W PARKING BLOCK-FOREVER SELECT ON PORT 3013 ...
//*   3. Submit TSTD1BV.  Wait for
//*        TSTD1B: V REQUEST ISSUED -- WAITING TO BE SERVED
//*   4. From the host:  nc 192.168.200.1 3013     (makes W's listener ready)
//*   5. FIXED    -> W SELECT COMPLETED RC=1, then V SERVED RC=0.
//*      UNFIXED  -> neither line ever appears; cancel both jobs and
//*                  P NSFS / S NSFS, because the parked SELCB survives the
//*                  client's cancel.
//*
//* POSITIVE CONTROL, and it is required: issue F NSFS,STATS during the window.
//* It must ANSWER -- the executive is not hung, only cross-AS service is
//* stalled -- otherwise "V was not served" cannot be told from "the STC died".
//*
//W       EXEC PGM=TSTD1B,PARM='W',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
