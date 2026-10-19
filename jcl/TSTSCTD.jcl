//TSTSCTD JOB (A),'NSF D2R2 DEAD',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* d2 round 2 (#136) -- DEAD: the S1 induction.
//*
//* Run ONLY after TSTRQXC PARM='LEAK' and a P NSFS that RETAINED
//* (NSF054W), with NSFS stopped.  In the anchor chained under 'nsfs'
//* it turns LEAK's one CLAIMED slot into a PENDING request whose
//* owner the death guard reads DEAD (a fabricated identity, ASID 1).
//* Then S NSFS must adopt the anchor and REAP that slot:
//*   NSF062I ... REAPED=1 ... INFLIGHT 1 -> 0
//*   NSF065I RETAINED SVC ROUTINE ... FREED
//* It refuses (CC 20) unless it finds exactly LEAK's state.
//*
//G       EXEC PGM=TSTSCTM,PARM='DEAD',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.DEV.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
