//TSTAPPDS PROC P=HANG
//*
//* 40-IDENT arm 2 -- the SAME client program the batch arms run, started as
//* a STARTED TASK instead of submitted as a job.
//*
//* WHY THIS EXISTS.  No test in this tree has ever watched a real address
//* space die.  TSTDEATH's DEAD rows stage a SYNTHETIC identity
//* (tstd_free_asid scans the ASVT for an ASID that is ALREADY available), and
//* every live client so far has been a batch job -- which runs in an
//* INITIATOR and therefore never takes its address space down with it
//* (M5-2c1 stage a).  So the ADR-0040 guard's DEAD path -- the case it was
//* built for, and the case M6 needs, since HTTPD and mvsMF are both STCs --
//* has never been exercised against a client whose address space actually
//* ended.
//*
//* An STC IS its own address space and does terminate.  Starting the
//* existing program rather than writing a new one is deliberate: the arm
//* must differ from the batch arms ONLY in how it is started, or a
//* difference in the reading cannot be attributed to that.
//*
//* Installation:
//*   Copy to SYS2.PROCLIB(TSTAPPDS)
//*
//* PREREQUISITE:  S NSFS
//*
//* Use:
//*   S TSTAPPDS            HANG -- stays alive ~15 min, then TERMAPIs itself
//*   P TSTAPPDS            stop it (it does not field a stop; use C for the
//*                         abrupt case)
//*   C TSTAPPDS            CANCEL -- the abrupt death
//*   S TSTAPPDS,P=CLEAN    control: INITAPI/SOCKET/TERMAPI, ends normally
//*   S TSTAPPDS,P=LEAVE    ends WITHOUT TERMAPI, leaving the slot registered
//*
//* Read the guard's verdict for it with  F NSFS,APPS  -- immediately after it
//* ends and again over the following minutes.  Cross-check the ASCB/ASID in
//* the NSF815I line against the TSTAPPD line the program WTOs before it
//* ends: a report about a different address space is not a measurement.
//*
//TSTAPPDS EXEC PGM=TSTAPPD,PARM='&P',REGION=8M
//STEPLIB  DD  DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD  DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD  SYSOUT=*
//SYSTSPRT DD  SYSOUT=*
//SYSUDUMP DD  SYSOUT=*
