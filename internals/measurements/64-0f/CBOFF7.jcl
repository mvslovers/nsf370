//CBOFF7   JOB (A),'CB OFFSETS 7',CLASS=A,MSGCLASS=H,
//             MSGLEVEL=(1,1),NOTIFY=&SYSUID
//*
//* 64-0f: ASCBSTOR is THE instrument of this round -- it is the field
//* that shows a COMPLETED swap cycle retrospectively, which is the only
//* way a 45-60 s cadence can see a fast out-and-back.  It was NOT
//* derived in CBOFF5/CBOFF6, so it is derived here from SYS1.AMODGEN
//* rather than taken from asread.py, which carries it unsourced.
//* The remaining ASCB fields the sampler reads are re-derived in the
//* same job so every offset in the sampler has a citation.
//*
//ASM     EXEC PGM=IFOX00,PARM='NODECK,NOLOAD,NORLD,NOXREF',REGION=4M
//SYSLIB   DD DSN=SYS1.AMODGEN,DISP=SHR
//         DD DSN=SYS1.MACLIB,DISP=SHR
//SYSUT1   DD UNIT=SYSDA,SPACE=(1700,(900,200))
//SYSUT2   DD UNIT=SYSDA,SPACE=(1700,(600,100))
//SYSUT3   DD UNIT=SYSDA,SPACE=(1700,(600,100))
//SYSPRINT DD SYSOUT=*
//SYSPUNCH DD DUMMY
CBOFF7   CSECT
ASTOR    DC    AL2(ASCBSTOR-ASCB)
AEJST    DC    AL2(ASCBEJST-ASCB)
ASWCT    DC    AL2(ASCBSWCT-ASCB)
AASID    DC    AL2(ASCBASID-ASCB)
AASXB    DC    AL2(ASCBASXB-ASCB)
ACPUS    DC    AL2(ASCBCPUS-ASCB)
ALEN     DC    AL2(ASCBLEN-ASCB)
         IHAASCB
         END
/*
