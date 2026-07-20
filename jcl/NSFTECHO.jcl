//NSFTECHO JOB (ACCT),'NSF TCP ECHO',CLASS=A,MSGCLASS=X,
//             MSGLEVEL=(1,1),REGION=4M,TIME=1440
//*
//* NSFTECHO - NSF TCP echo server sample (milestone M4-5, spec ch. 15).
//*
//* The stream sibling of NSFECHO: a TCP echo server over the EZASOKET C API.
//* It brings up the Phase-1 stack (CTCI + IP + TCP), listens on a TCP port and
//* echoes every byte back on each accepted connection until the peer closes
//* (EOF -> close -> back to accept). Drive it from the host with
//*   telnet 192.168.200.1 <port>
//* Type lines; they echo back. A line beginning "QUIT" ends the server.
//*
//* Install:   make deploy                 -> NSF.LINKLIB(NSFTECHO)
//* Submit:    this member (PARM = TCP port; default 7, the echo port)
//*            //ECHO EXEC PGM=NSFTECHO,PARM='3007'   for another port
//* Stop:      send a "QUIT" line (clean), or CANCEL NSFTECHO. The server blocks
//*            in accept/recv while idle and burns ~no CPU, so TIME= will not end
//*            it -- QUIT (clean) or CANCEL is the way out.
//*
//* Output:    SYSPRINT carries the step log (INITAPI/BIND/LISTEN lines, the
//*            per-connection accepts, the echoed count and the leak gates);
//*            device-up messages (NSF210I/211I) go to the console. A SYSUDUMP
//*            means an unexpected abend -- there should be none.
//*
//ECHO     EXEC PGM=NSFTECHO,PARM='7'
//STEPLIB  DD  DISP=SHR,DSN=NSF.LINKLIB
//SYSPRINT DD  SYSOUT=*
//SYSTERM  DD  SYSOUT=*
//SYSUDUMP DD  SYSOUT=*
