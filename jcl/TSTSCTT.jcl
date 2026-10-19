//TSTSCTT JOB (A),'NSF D2R2B TAIL',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* d2 round 2b -- TAIL: repair an anchor a pre-2b build registered.
//*
//* Run ONLY with NSFS stopped and an anchor RETAINED under 'nsfs'
//* whose jobname_tail is four zero bytes (round 1 and round 2 never
//* wrote it).  It writes NSFS's tail, four blanks, and nothing else.
//* THE RIG WRITES THE TAIL: the adoption that follows is real, the
//* identity it adopts by was supplied by this job.  Then S NSFS:
//*   NSF061I RENDEZVOUS FOUND A RETAINED ANCHOR=... -- ADOPTING
//* It refuses (CC 20) unless the tail is exactly 00000000.
//*
//G       EXEC PGM=TSTSCTM,PARM='TAIL',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.DEV.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
