//TSTRQXCB JOB (A),'NSF POOL B',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2b4 -- the two-client contention gate, FOLLOWER half.
//*
//* Submit this and TSTRQXCA.jcl TOGETHER, A first.  Each raises a flag in the
//* CSA pool and waits up to 60 s for the other; if only one is submitted it
//* times out and returns CC 20 ("the gate could not run"), never CC 0.
//*
//* B creates the contention A measures.  It hammers the pool for exactly as
//* long as A's flag is up, and asserts only what is true of it regardless of
//* phase: every request it was served came back carrying ITS OWN identity, so
//* no slot was ever handed to two clients.
//*
//* PREREQUISITE:  S NSFS   (and nothing else driving the transport)
//* RECOVERY:      run TSTRQXC with PARM='RESET' if a run dies mid-phase,
//*                or recycle the STC (P NSFS / S NSFS).
//*
//B       EXEC PGM=TSTRQXC,PARM='B',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
