//TSTAPPDP JOB (A),'NSF APP PARK',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* 40-CHK -- the PARK arm: a batch client that dies WITH A REQUEST
//* OUTSTANDING, which is the only state in which the ADR-0040 client-death
//* guard has anything to protect.
//*
//* It binds port 7799 and blocks in RECVFROM.  Nothing sends to that port, so
//* the request stays published and this task waits in the SVC routine on the
//* CSA reply ECB until an operator cancels it:
//*
//*     C TSTAPPDP
//*
//* THEN send one datagram to port 7799 to COMPLETE the parked request, which
//* is what drives the STC to the guard and the reply POST:
//*
//*     ssh mvsdev "printf x | nc -u -w1 192.168.200.1 7799"
//*
//* RISK, and the recovery ladder is in docs/measurements/40-chk/predictions.md:
//* the task is cancelled in supervisor state, key 0, inside the SVC routine,
//* holding a CSA slot.  VERIFY IT ACTUALLY TERMINATED (D A,L and the jobid in
//* OUTPUT) before doing anything else.  P NSFS is NOT a recovery path -- its
//* nudge POSTs parked clients, and this one is dead.
//*
//* This arm leaks one app slot and one CSA request slot; P NSFS / S NSFS
//* resets both.
//*
//* PREREQUISITE:  S NSFS
//*
//RUN     EXEC PGM=TSTAPPD,PARM='PARK',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
