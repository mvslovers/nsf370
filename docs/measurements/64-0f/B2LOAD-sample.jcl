//* 64-0f B2b sustained-load job -- GENERATED, 250 identical steps.
//* MVS 3.8j caps a job at 255 EXEC statements (IEF602I EXCESSIVE NUMBER
//* OF EXECUTE STATEMENTS); the first attempt used 400 and every job died
//* JCL ERROR while the anchor read 'served' frozen -- a load arm that was
//* not loading.  14 such jobs x 250 steps x 8 requests = 28 000 requests.
//* SYSPRINT/SYSTSPRT DUMMY + MSGLEVEL=(0,0): the measure is the anchor's
//* 'served' counter read through /.dm, not the spool (64-0e's $HASP355).
//*
//B2LOAD01 JOB (A),'NSF 64-0F B2',CLASS=A,MSGCLASS=H,MSGLEVEL=(0,0)
//S001 EXEC PGM=TSTRQXC,REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD DUMMY
//SYSTSPRT DD DUMMY
//SYSUDUMP DD DUMMY
//S002 EXEC PGM=TSTRQXC,REGION=8M
//*  ... S003 .. S250 identical ...
