# M5-2c1 stage a -- predictions, recorded BEFORE the arms ran

Written before the CLEAN and CANCEL arms were submitted, so that a
confirmation is a measurement and not a memory (the 64-3-0 / 64-0f
convention). The LEAVE arm had already run; its result is quoted here only
because the later predictions follow from it.

## Already measured (LEAVE arm, 2026-08-28T06:44:50Z)

Two app instances left registered by a job that ended normally without
TERMAPI both classify **LIVE**, not DEAD and not UNKNOWN, seven seconds after
the job reached OUTPUT with CC 0000. Both carry ASCB=00FE7B58 ASID=0006 --
the pair the job itself WTOed before ending.

## P1 -- CLEAN arm

The registry returns to 0 of 16 in use for that instance, and the job reports
**the same ASCB=00FE7B58 ASID=0006** as the LEAVE arm.

If the pair repeats, the address space recorded at INITAPI is the INITIATOR,
which outlives the job and is handed to the next one -- so a recorded identity
can go on answering LIVE while a different application entirely is running
there. That is the sharpest form of the finding and the reason P1 is worth
stating in advance: a repeat is not a coincidence to be noticed afterwards.

## P2 -- CANCEL arm

**LIVE, exactly as the LEAVE arm.** A cancelled batch job frees its initiator
the same way a normally-ended one does: the job goes, the address space stays.
If the mechanism is the initiator, CANCEL cannot differ from LEAVE, and the
operator-driven case is not a second case at all.

Refuted if CANCEL reads DEAD or UNKNOWN -- which would mean job termination,
not address-space termination, reaches the ASVT, and would make the arm's
distinction real.

## P3 -- the decay poll

No change over six minutes. An initiator does not terminate because a job
ended, so there is nothing to decay. A DELAYED transition to DEAD would refute
this and would bound what a sweep could promise.
