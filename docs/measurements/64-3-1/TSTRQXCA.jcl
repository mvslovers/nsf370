//TSTRQXA JOB (A),'NSF 64-3-1 GATE A',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* 64-3-1 Stage B gate -- the two-client contention round that reproduced
//* the #64 stall twice within 90 s (docs/nsf-64-0c-measurements.md:189-196).
//*
//S       EXEC PGM=TSTRQXC,REGION=8M,PARM='A'
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
