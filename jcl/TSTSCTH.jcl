//TSTSCTH JOB (A),'NSF D2R1 HOLD',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* d2 round 1 (ADR-0048) -- HOLD: chain NSFS's derived SSCT, leave it.
//*
//* The derived key for job NSFS is 'nsfs' (lower case, ADR-0048 6).
//* HOLD chains it pointing into this job's own storage and ENDS,
//* leaving the SSCT behind -- the state round 1 failed in, made
//* WITHOUT a retained anchor, so it costs no CSA until IPL.  Then:
//*
//*   S NSFS   must START (round 1 failed it IEF612I), and say NSF059W
//*            (it refuses to chain a second SSCT under the same name)
//*   P NSFS   takes the free path, which removes the SSCT by name
//*   TSTSCTP  then reports it ABSENT
//*
//* It self-authorizes (SVC 244) because ssct_new/ssct_install need
//* supervisor state, exactly as they do inside NSFS.
//*
//G       EXEC PGM=TSTSCTM,PARM='HOLD',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
