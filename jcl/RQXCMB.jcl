//RQXCMB   JOB (A),'NSF MEASURE MB',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2e transport baseline -- TWO ADDRESS SPACES, follower half.
//*
//* See RQXCMA.jcl.  Neither half is reliably the last one out of a
//* timed window, so the pool-clean count is REPORTED by both and
//* asserted by neither: that check belongs to whoever leaves last, and
//* in a timed window that is not known in advance.
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
//M       EXEC PGM=TSTRQXC,PARM='MB',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
