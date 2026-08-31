//RQXCMP   JOB (A),'NSF MEASURE MSP',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2e -- the discriminating gate on the INSTRUMENT, not on the
//* transport.  MSP pauses inside the timed region, so the instrument
//* must report at least that much or it is broken.  It asserts min,
//* mean AND that every bucket below 5 ms is EMPTY -- min/max alone
//* would pass with the BUCKETING broken.
//*
//* Run it once per round BEFORE the measurement windows: a latency
//* figure that is simply wrong looks exactly like a fast transport.
//*
//* THE WINDOW IS 300 SECONDS AND THE FIRST 60 ARE DISCARDED FROM THE
//* FIGURES, NOT FROM THE RUN: both regions are printed, so the discard
//* is auditable.  A PARM with a trailing decimal shortens the window
//* and stamps the run a TRIAL -- it never scales the discard, and a
//* measurement run must NOT use one.
//*
//* PREREQUISITE: S NSFS (and nothing else driving the transport).
//* RECOVERY:     TSTRQXC with PARM='RESET' if a run dies mid-window,
//*               or recycle the STC (P NSFS / S NSFS).
//*
//M       EXEC PGM=TSTRQXC,PARM='MSP',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
