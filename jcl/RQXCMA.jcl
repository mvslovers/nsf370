//RQXCMA   JOB (A),'NSF MEASURE MA',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2e transport baseline -- TWO ADDRESS SPACES, leader half.
//*
//* Submit this and RQXCMB.jcl TOGETHER.  Each raises a flag in the CSA
//* pool and waits up to 60 s for the other; if only one is submitted
//* it times out and returns CC 20, never CC 0.
//*
//* Both halves stop on their OWN clock, so the windows are offset by
//* the barrier poll.  Each prints its opening and closing TOD and the
//* two are directly comparable (one hardware clock), so the skew is a
//* MEASURED quantity rather than an assumption.
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
//M       EXEC PGM=TSTRQXC,PARM='MA',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
