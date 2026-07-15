//NSFECHO  JOB (ACCT),'NSF UDP ECHO',CLASS=A,MSGCLASS=X,
//             MSGLEVEL=(1,1),REGION=4M,TIME=1440
//*
//* NSFECHO - NSF UDP echo server sample (milestone M3-5, spec ch. 15).
//*
//* The first user-visible NSF program: a UDP echo server over the EZASOKET
//* C API. It brings up the Phase-1 stack (CTCI + IP + UDP), binds a UDP port
//* and echoes every datagram back to its sender until a 4-byte "QUIT"
//* datagram arrives. Drive it from the host with samples/host/echo_client.py.
//*
//* Install:   make deploy                 -> NSF.LINKLIB(NSFECHO)
//* Submit:    this member (PARM = UDP port; default 7, the echo port)
//*            //ECHO EXEC PGM=NSFECHO,PARM='7777'   for another port
//* Stop:      send a "QUIT" datagram (echo_client.py quit), or CANCEL NSFECHO.
//*            The server blocks in recvfrom while idle and burns ~no CPU, so
//*            TIME= will not end it -- QUIT (clean) or CANCEL is the way out.
//*
//* Output:    SYSPRINT carries the step log (INITAPI/BIND lines, the echoed
//*            count and the leak gates); device-up messages (NSF210I/211I) go
//*            to the console. A SYSUDUMP means an unexpected abend -- there
//*            should be none.
//*
//ECHO     EXEC PGM=NSFECHO,PARM='7'
//STEPLIB  DD  DISP=SHR,DSN=NSF.LINKLIB
//SYSPRINT DD  SYSOUT=*
//SYSTERM  DD  SYSOUT=*
//SYSUDUMP DD  SYSOUT=*
