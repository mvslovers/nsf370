//NSFS     PROC M=NSFPRM0,
//            D='SYS2.PARMLIB'
//*
//* NSFS - the Phase-2 NSF stack STC (M5-2a, ADR-0041).
//*
//* The same executive as NSF, in its own address space, reached by
//* applications through the private SVC (ADR-0038) instead of a local
//* subtask queue.  Requests cross as the frozen 64-byte NSFRQE through a
//* CSA request slot; the executive dispatches an STC-PRIVATE copy of it.
//*
//* NSFS and NSFV are ALTERNATIVES, never concurrent: both steal the same
//* installation SVC slot.  Stop one before starting the other.
//*
//* Installation:
//*   Copy to SYS2.PROCLIB(NSFS)            (cataloged proc; no // PEND)
//*   Deploy the load modules:  make deploy  -> NSF.LINKLIB
//*     (NSFS = this STC, NSFVSVC = the CSA-resident SVC routine it __loadhi's
//*      and points a stolen SVCTABLE slot at)
//*   The STC self-authorises at runtime via clib_apf_setup (SVC 244), so
//*   NSF.LINKLIB need not be APF.  The CLIENT is NOT authorised.
//*
//* Starting:    /S NSFS
//* Commands:    /F NSFS,DISPLAY | STATS | TRACE comp ON|OFF | STOP
//* Stopping:    /P NSFS         (restores the SVC slot; allow ~30s for the
//*                               in-flight drain before assuming a hang)
//*
//* Client:      make test-mvs ARGS="--only TSTRQXM"   (NSFS must be started)
//*
//NSFS     EXEC PGM=NSFS,REGION=4M,TIME=1440
//STEPLIB  DD  DISP=SHR,DSN=NSF.LINKLIB
//SYSUDUMP DD  SYSOUT=*
//SYSPRINT DD  SYSOUT=*
//PROFILE  DD  DSN=&D(&M),DISP=SHR,FREE=CLOSE
