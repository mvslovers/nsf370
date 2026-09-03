//RQX2A   JOB (A),'NSF TWO-CLIENT A',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2e JOB A -- the exit gate.  TWO ADDRESS SPACES, role A.
//*
//* PASS/FAIL.  It reports NO timing figures: the per-interval counts
//* it prints are PROGRESS evidence for multiplexing and are NOT
//* comparable with anything Job B (TSTRQXC MA/MB) produces.
//*
//* Submit this and RQX2B.jcl TOGETHER.  Each raises a flag in the CSA
//* slot pool and waits up to 60 s for the other; a job submitted on
//* its own times out and returns CC 20 -- "the gate could not run" --
//* never CC 0.
//*
//* PREREQUISITE: S NSFS, FRESHLY STARTED, with nothing else holding
//* sockets.  The gate aims at an EXACT descriptor value rather than a
//* derived one, and that value is exact only on a fresh socket table
//* (soc_init seeds every slot generation to 1; sock_alloc hands out
//* the lowest free index).  A stale table is a SKIP (CC 20), not a
//* failure.  The interface must also be up (NSF210I / NSF211I): one
//* datagram per checkpoint really goes on the wire, and EMSGSIZE
//* itself is decided in udp_send only after nsfip_route has found a
//* device to take the MTU from.
//*
//* RECOVERY: a run that dies mid-window leaves its flag slots HELD.
//*           TSTRQXC with PARM='RESET' releases every CLAIMED/HELD
//*           slot, or recycle the STC (P NSFS / S NSFS).
//*
//G       EXEC PGM=TSTRQX2,PARM='A',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
