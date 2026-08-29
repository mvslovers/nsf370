//TSTRQXR  JOB (A),'NSF 80-CHK RECV',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* 80-CHK -- does a data-returning cross-AS receive store into key-0 CSA
//* from the executive's key 8?  (issue #80)
//*
//* THIS JOB IS EXPECTED TO ABEND NSFS (S0C4) AND THEN HANG.
//*
//* The client parks in WAIT on SLRECB in the retained CSA anchor, and
//* recovery does not nudge parked clients -- so nobody will ever POST it.
//* CANCEL IT (C TSTRQXR) once the console has shown the verdict.  The S222
//* takes buffered SYSPRINT with it, which is why every step of the probe is
//* marked with a WTO: the markers survive the cancel and ARE the result.
//*
//* Read the console for:
//*    TSTRQXR: control 2 -- ZERO-BYTE RECV RETURNED n=0    <- the control
//*    TSTRQXR: ARM -- DATA RECV ISSUED ...                 <- last marker
//*    (no "ARM -- DATA RECV RETURNED")                     <- the verdict
//*    IEF450I NSFS NSFS - ABEND S0C4
//*
//* COST: one arm abends NSFS and leaks ~139 KB of CSA (the anchor plus the
//* retained SVC router) until the next IPL.  Read NSF055I's "LARGEST FREE
//* BLOCK NOW" at each S NSFS to track it.
//*
//* TWO ARMS, selected by PARM:
//*
//*   PARM='ARM'   UDP (80-CHK).  Zero-byte control then the data arm.
//*                peer: python3 recvkey_peer.py 192.168.200.2 3004 --len 256
//*
//*   PARM='ARMT'  TCP (80-FIX).  The same store through src/nsftcp.c -- the
//*                path HTTPD and mvsMF would use at M6, so it is measured on
//*                both sides of the fix rather than reasoned.
//*                peer: python3 recvkey_peer.py 192.168.200.2 3005 --tcp
//*                                             --len 256
//*
//* AFTER THE FIX both arms are expected to COMPLETE; before it, both abend.
//*
//* PREREQUISITES:
//*   S NSFS
//*   the peer for the arm you are running (above)
//*
//ARM     EXEC PGM=TSTRQXR,PARM='ARM',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
