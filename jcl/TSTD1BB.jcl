//TSTD1BB JOB (A),'NSF D1 GATE B',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2d1 live gates 2.2/2.3 -- ROLE B.
//*
//* TWO REAL ADDRESS SPACES.  Submit TSTD1BA first, wait for its console line
//*   TSTD1B: A HOLDING SOCKET ... -- B MAY RUN NOW
//* then submit TSTD1BB.  A holds its socket ~60 s, which is B's window.
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
