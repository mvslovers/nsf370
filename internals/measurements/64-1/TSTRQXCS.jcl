//TSTRQXCS JOB (A),'NSF POOL SOLO',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* 64-1 -- TSTRQXC SOLO (no PARM): 8 sequential requests, one client.
//* Used as the Gate 1 workload: it makes the instance serve requests, which
//* is the only variable between the two halves of the controlled pair.
//*
//S       EXEC PGM=TSTRQXC,REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
