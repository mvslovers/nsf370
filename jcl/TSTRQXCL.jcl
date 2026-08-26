//TSTRQXCL JOB (A),'NSF POOL LEAK',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* M5-2b4 -- the RETAIN-BRANCH INDUCTION.  RUN THIS LAST IN A ROUND.
//*
//* It leaves one CSA slot CLAIMED with the in-flight count leaked and
//* DELIBERATELY DOES NOT CLEAN UP, so the next `P NSFS` has to choose between
//* retaining the CSA and freeing it under a client -- the branch b3 built and
//* never executed.
//*
//* A healthy client parked in the SVC routine cannot induce this: `nsfsx_stop`
//* clears ANCHOR_ACTIVE before it nudges, so a nudged client takes the
//* routine's WQUIES path, frees its own slot and gives the count back.  That
//* is why a blocking RECV drains itself and lands in the drained branch.
//*
//* THE SEQUENCE:
//*   1. S NSFS
//*   2. submit this job          -- it prints and WTOs ANCHOR=xxxxxxxx
//*   3. P NSFS                   -- expect NSF054W ... CSA AND SVC ROUTINE
//*                                  RETAINED, and NO router unload
//*   4. run TSTRQXC PARM='Vxxxxxxxx' with the address from step 2 -- the
//*      anchor must still carry its eyecatcher (nsfsx_anchor_free zeroes it
//*      before freeing, so an intact eye means the free path was not taken)
//*   5. S NSFS                   -- a fresh anchor; the old 134 KB and the
//*                                  routine leak until IPL, which is the
//*                                  cheap side of the safe-side asymmetry
//*
//L       EXEC PGM=TSTRQXC,PARM='LEAK',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
