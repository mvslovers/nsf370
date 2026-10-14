//TSTSCTG JOB (A),'NSF D2R1 GATE',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* d2 round 1 (ADR-0048) -- the two-name gate, on the real SSI chain.
//*
//* Registers NSZ1 and NSZ2, finds each under its own name, shows neither
//* finds the other's, removes each by name, and asserts nothing is left
//* chained.  THROWAWAY NAMES: they belong to no product, so a live NSFS's
//* own registration is never touched.
//*
//* TWO NAMES, NOT TWO STACKS.  Only one instance can steal SVC 239, so a
//* second NSFS is not available -- and the rendezvous does not need one.
//* This proves the registration is keyed by a NAME rather than a place; it
//* does NOT prove two stacks can run.
//*
//* It self-authorizes (SVC 244) because ssct_new/ssct_install need supervisor
//* state, exactly as they do inside NSFS, which is ac=1.
//*
//G       EXEC PGM=TSTSCTM,PARM='GATE',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
