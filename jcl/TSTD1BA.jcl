//TSTD1BA JOB (A),'NSF D1 GATE A',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2d1 live gates 2.2/2.3 -- ROLE A.
//*
//* TWO REAL ADDRESS SPACES.  Submit TSTD1BA first, wait for its console line
//*   TSTD1B: A HOLDING SOCKET ... -- B MAY RUN NOW
//* then submit TSTD1BB.  A holds ~180 s, polling its OWN readiness, which is
//* B's window.
//*
//* A THIRD PARTY IS REQUIRED: the stimulus driver
//* docs/measurements/m5-2d1-stimulus/d1stim.py, running on the HOST beside
//* the emulator.  It connects to A's port and HOLDS the connection open, so
//* A's listener becomes read-ready -- which is what makes B's "not ready"
//* a refusal rather than an idle listener (the #107 annotation).
//*
//* SUBMIT B ON A's *HOLDING* LINE, NOT ON ITS FIRST READY LINE.  A cannot be
//* ready until after B has swept: a passive child takes a socket-table slot,
//* so a connect before B allocates its own socket would insert a child between
//* A and B and break B's `own - 1` derivation -- silently, because the child
//* is foreign to B too.  The driver therefore fires on B's own SWEEP DONE
//* marker, and A's not-ready polls before that point are the expected reading.
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
//A       EXEC PGM=TSTD1B,PARM='A',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
