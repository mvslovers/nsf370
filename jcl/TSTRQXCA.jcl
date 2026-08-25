//TSTRQXCA JOB (A),'NSF POOL A',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2b4 -- the two-client contention gate, LEADER half.
//*
//* Submit this and TSTRQXCB.jcl TOGETHER, A first.  Each raises a flag in the
//* CSA pool and waits up to 60 s for the other; if only one is submitted it
//* times out and returns CC 20 ("the gate could not run"), never CC 0.
//*
//* A owns the assertions, because it is the only party that knows what phase
//* the pool is in.  Its decisive one: in phase 2 it pre-claims every slot but
//* ONE, and must be REFUSED at least once -- which can only happen if the
//* OTHER ADDRESS SPACE is holding that slot at that instant.
//*
//* PREREQUISITE:  S NSFS   (and nothing else driving the transport)
//* RECOVERY:      run TSTRQXC with PARM='RESET' if a run dies mid-phase,
//*                or recycle the STC (P NSFS / S NSFS).
//*
//A       EXEC PGM=TSTRQXC,PARM='A',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
