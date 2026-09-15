//TSTD1BB JOB (A),'NSF D1 GATE B',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2d1 live gates 2.2/2.3 -- ROLE B.
//*
//* TWO REAL ADDRESS SPACES.  Submit TSTD1BA first, wait for its console line
//*   TSTD1B: A HOLDING SOCKET ... -- B MAY RUN NOW
//* then submit TSTD1BB.  A holds ~180 s, polling its OWN readiness.
//*
//* B ANNOUNCES ITS OWN STIMULUS.  After the sweep and its own socket, B emits
//*   TSTD1B: B SWEEP DONE A-DESC ... -- CONNECT TO A NOW
//* and waits 8 s.  The host driver (d1stim.py) connects to A on that marker,
//* which makes A's listener read-ready; it connects twice more inside B's
//* 20 s park, so the arm-2 re-scan path is driven by a real readiness EDGE.
//* Without that driver both SELECT arms are unreadable -- they cannot tell
//* "refused" from "resolved and idle", which is exactly what the 2026-09-03
//* annotation on docs/measurements/m5-2-d1-select/ says.
//*
//* B sweeps the WHOLE internal descriptor space (gen<<16)|idx straight into
//* the request, bypassing its own facade table -- the facade cannot NAME a
//* foreign socket, the transport could.  It reports ATTEMPTS as well as hits:
//* "0 hits" alone is a null nobody can read, "0 hits in 128 attempts with A
//* confirmed holding a socket" is evidence.
//*
//* PREREQUISITE:  S NSFS, and TSTD1A (gate 2.1) green first -- a check that
//*                refuses accepted children makes everything here moot.
//*
//B       EXEC PGM=TSTD1B,PARM='B',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
