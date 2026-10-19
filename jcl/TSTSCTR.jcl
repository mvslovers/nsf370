//TSTSCTR JOB (A),'NSF D2R1 RM SSCT',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* d2 round 1 -- RECOVERY AID.  Removes the SSCT named in the PARM.
//*
//* WHY IT EXISTS: this round found live that a chained SSCT whose name equals
//* a PROCEDURE name makes `S <name>` fail IEF612I PROCEDURE NOT FOUND -- MVS
//* resolves the name as a subsystem before it looks for a procedure.  After a
//* retain-path stop the registration survives by design, so the STC cannot be
//* started again until it is removed.  Without this job the only way back is
//* an IPL.
//*
//G       EXEC PGM=TSTSCTM,PARM='RMNSFS',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.DEV.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
