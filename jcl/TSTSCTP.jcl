//TSTSCTP JOB (A),'NSF D2R1 PROBE',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* d2 round 1 (ADR-0048) -- the PROBE half of the retain/free check.
//*
//* Reports whether an SSCT is chained under NSFS's derived name, and the
//* SSCTSUSE it carries.  A REPORT, NOT AN ASSERTION: whether it should be
//* chained right now depends on which stop path the STC last took, which this
//* program cannot know.  The round compares two PROBE runs either side of a
//* P NSFS:
//*
//*   after a CLEAN stop (the free path)   -> ABSENT   (removed with the storage)
//*   after a RETAIN     (NSF054W)         -> CHAINED  (the pointer must outlive
//*                                                     the STC, or a later start
//*                                                     cannot find the anchor)
//*
//* Getting that asymmetry backwards would reinstate the per-recycle leak while
//* every message on the retain path still read correctly, which is why it is
//* checked here rather than trusted to a comment in nsfsx_stop.
//*
//* PROBE needs NO authorization: reading the SSI chain is a fetch of common
//* storage.  Only register/remove enter supervisor state.
//*
//G       EXEC PGM=TSTSCTM,PARM='PROBE',REGION=8M
//STEPLIB  DD DSN=IBMUSER.NSF370.V0R1M0D.TESTLIB,DISP=SHR
//         DD DSN=NSF.DEV.LINKLIB,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTSPRT DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
