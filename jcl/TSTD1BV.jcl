//TSTD1BV JOB (A),'NSF 101 ARM3 VICTIM',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* #101 Stage 2, ARM 3 -- ROLE V (the victim).  Submit ONLY after the console
//* shows  TSTD1B: W PARKING BLOCK-FOREVER SELECT ...  from TSTD1BW.
//*
//* V issues one ordinary cross-AS request (INITAPI) while W is parked, and
//* marks the console before and after.  Served or not served IS the whole
//* observation, and both markers are WTOs because on the unfixed module this
//* job hangs and loses its buffered SYSPRINT to the cancel.
//*
//*   FIXED    TSTD1B: V REQUEST ISSUED ... then TSTD1B: V SERVED RC=0
//*   UNFIXED  the ISSUED line only; V never returns.
//*
//* See TSTD1BW's prologue for the full order and the F NSFS,STATS control.
//*
//V       EXEC PGM=TSTD1B,PARM='V',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
