//NSFP     PROC
//*
//* NSFP - M5 Stage-0a SSI cross-AS transport probe STC (ADR-0036).
//*
//* A do-nothing SSI subsystem that round-trips a 32-bit token from a client
//* address space to this STC and back, to prove the Phase-2 cross-AS transport
//* mechanics before M5-2 rides the real NSFRQE over them.  NOT the production
//* stack: the probe subsystem name is NSFP (NSFS is reserved for M5-2).
//*
//* Installation:
//*   Copy to SYS2.PROCLIB(NSFP)            (cataloged proc; no // PEND)
//*   Deploy the load modules:  make deploy  -> NSF.LINKLIB
//*     (NSFP = this STC, NSFPSSIR = the CSA-resident SSI router it __loadhi's)
//*   The task self-authorizes at runtime via clib_apf_setup (SVC 244), so
//*   NSF.LINKLIB need not be APF-authorized.
//*
//* Starting:    /S NSFP
//* Commands:    /F NSFP,STATS     (report served + in-flight counters)
//* Stopping:    /P NSFP           (deregisters the SSCT; /S NSFP works again --
//*                                 the double-start test)  or  /F NSFP,STOP
//*
//* Client:      make test-mvs ARGS="--only TSTSSI"   (NSFP must be started)
//*
//NSFP     EXEC PGM=NSFP,REGION=4M,TIME=1440
//STEPLIB  DD  DISP=SHR,DSN=NSF.LINKLIB
//SYSUDUMP DD  SYSOUT=*
//SYSPRINT DD  SYSOUT=*
