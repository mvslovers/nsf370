//TSTAPPDH JOB (A),'NSF APP HANG',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2c1 stage a -- the HANG arm: stays alive to be CANCELled -- the operator-driven case.
//* Bounded at 15 minutes; a run that reaches the ceiling WITHOUT being
//* cancelled cleans up, says so on the console, and returns CC 20.
//*
//* THE MEASUREMENT.  Reclaiming a leaked app slot means classifying its owner
//* DEAD, and ADR-0040 never reaps an UNKNOWN.  So the question that decides
//* whether obligation #3 can work at all is what the guard says about an
//* address space that ended NORMALLY.  Read the answer with:
//*
//*     F NSFS,APPS
//*
//* immediately after the job ends, and again over the following minutes -- a
//* DELAY bounds what the feature can promise, and only repeated readings see
//* one.  Cross-check the ASCB/ASID in the NSF815I line against the TSTAPPD
//* line this job WTOs before it ends: a report about a different address
//* space is not a measurement.
//*
//* Each LEAVE / HANG run costs ONE app slot until something reclaims it --
//* that is the point.  The registry is 16 slots and the seventeenth INITAPI
//* fails EMFILE; P NSFS / S NSFS resets it (the registry is STC-private).
//*
//* PREREQUISITE:  S NSFS
//*
//RUN     EXEC PGM=TSTAPPD,PARM='HANG',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
