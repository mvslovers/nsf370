//NSFV     PROC
//*
//* NSFV - M5 Stage-0a' SVC cross-AS transport probe STC (ADR-0038).
//*
//* A do-nothing probe that round-trips a 32-bit token from a client address
//* space to this STC and back over a PRIVATE SVC, to prove the Phase-2 cross-AS
//* transport mechanics with an UNAUTHORIZED client -- the thing the SSI probe
//* (NSFP, ADR-0036) could not do.  NOT the production stack (NSFS is reserved
//* for M5-2).  Supersedes NSFP's SSI transport (ADR-0038); NSFP stays in-repo
//* as the SSI reference.
//*
//* Installation:
//*   Copy to SYS2.PROCLIB(NSFV)            (cataloged proc; no // PEND)
//*   Deploy the load modules:  make deploy  -> NSF.LINKLIB
//*     (NSFV = this STC, NSFVSVC = the CSA-resident SVC routine it __loadhi's
//*      and points a stolen SVCTABLE slot at)
//*   The STC self-authorizes at runtime via clib_apf_setup (SVC 244) for
//*   __loadhi / key-0 CSA / the SVCTABLE store, so NSF.LINKLIB need not be APF.
//*   The CLIENT is NOT authorized (ADR-0038 red line).
//*
//* Starting:    /S NSFV     (steals an unused installation SVC slot; the slot
//*                           is RESTORED at stop AND on abend)
//* Commands:    /F NSFV,STATS     (report served + in-flight counters)
//* Stopping:    /P NSFV           (restores the SVC slot)  or  /F NSFV,STOP
//*
//* Client:      make test-mvs ARGS="--only TSTSVC"   (NSFV must be started)
//*
//NSFV     EXEC PGM=NSFV,REGION=4M,TIME=1440
//STEPLIB  DD  DISP=SHR,DSN=NSF.LINKLIB
//SYSUDUMP DD  SYSOUT=*
//SYSPRINT DD  SYSOUT=*
