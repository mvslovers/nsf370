# M5-2c1 stage a -- the measurement, and why stage b did not start

**Stand:** MVSCE on Hercules (`mvsdev`), NSFS STC 1504, module deployed
2026-08-28 from `m5-2c1-owner-sweep`. Client `TSTAPPD`, unauthorised
(`TESTAUTH FCTN=1 == 0`, asserted). All times UTC.

## The question

M5-2c1 carries obligation #3: an application address space that dies without
calling TERMAPI should have its sockets reclaimed automatically. Reclaiming
means classifying its owner **DEAD**, and ADR-0040's rule is absolute --
UNKNOWN is never reaped, because a live client called dead has its storage
freed underneath it.

So one fact decides whether the sweep can work: **what does
`nsfreqx_classify` say about an address space whose job has ended?** Nobody
had established it. The kickoff listed three possible answers -- DEAD
promptly, UNKNOWN, or DEAD after a delay -- and made the round stop and report
on anything but the first.

## The answer: LIVE

Not one of the three. **LIVE**, seven seconds after the job reached OUTPUT
with CC 0000, and still LIVE six and a half minutes later.

| arm | job | ended | verdict |
|---|---|---|---|
| LEAVE (no TERMAPI) | JOB02807 MBTTEST, batch + TSO steps | CC 0000, OUTPUT | **LIVE** |
| CANCEL | JOB02809 TSTAPPDH | ABEND S222, OUTPUT | **LIVE** |
| CLEAN (control) | JOB02808 TSTAPPDC | CC 0000, OUTPUT | slot released by TERMAPI |

## Why: the recorded ASCB is the INITIATOR's, and the initiator outlives the job

Every arm reported **`ASCB=00FE7B58 ASID=0006`** -- three different jobs, one
address space. A batch job does not get an address space of its own; it runs
in an initiator, and the initiator does not terminate when the job ends. It
waits for the next job and is handed to it.

That is proven live rather than inferred: `TSTAPPDC` (JOB02808) is a
*different job* submitted after `MBTTEST` (JOB02807) had reached OUTPUT, and
it announced the same ASCB and the same ASID. `TSTAPPDH` (JOB02809) made it
three.

So the ASVT entry for ASID 0006 never goes AVAILABLE, its ASCB never changes,
and `nsfreqx_classify` correctly answers LIVE. **The classifier is not
wrong.** It answers the question it was built to answer -- "did that address
space end?" -- and for a batch client the answer is no, even though the
application is gone.

### The consequence is worse than "reclaims nothing"

A sweep gated on DEAD would reclaim nothing for batch clients. But the same
reading says something sharper: because the initiator is *reused*, a recorded
identity keeps answering LIVE while **a different application entirely** is
running in that address space. The identity does not merely fail to expire;
after the next job starts it no longer denotes what it was recorded for.

### The failure direction is the safe one

A false LIVE leaks an app slot. It never tears down a healthy application's
sockets. That is ADR-0040's safe-side asymmetry holding under a condition
nobody had anticipated -- worth stating, because the finding is a blocked
feature and not a live defect.

## Evidence

**Cross-check, per reading.** The client WTOs its own ASCB/ASID (`__ascb(0)`,
`ASCBASID` at ASCB+X'24') before it ends, so every report line is tied to a
job that demonstrably ended rather than to some other address space:

```
1.44.39 JOB 2807  +TSTAPPD: LEAVE ARM -- ASCB=00FE7B58 ASID=0006 SOCKET=0
1.47.54 JOB 2808  +TSTAPPD: CLEAN ARM -- ASCB=00FE7B58 ASID=0006 SOCKET=0
1.48.14 JOB 2809  +TSTAPPD: HANG  ARM -- ASCB=00FE7B58 ASID=0006 SOCKET=0
```

**First reading, T+7s** (job ended 06:44:39Z, read 06:44:50Z) -- taken this
fast on purpose: a first sample minutes later cannot distinguish "LIVE
throughout" from "DEAD immediately, then the ASID reused".

```
NSF814I APP REGISTRY:
NSF815I   SLOT  0 TOKEN=00010000 ASCB=00FE7B58 ASID=0006 LIVE
NSF815I   SLOT  1 TOKEN=00010001 ASCB=00FE7B58 ASID=0006 LIVE
NSF816I APP REGISTRY: 2 OF 16 SLOTS IN USE, 0 DEAD
```

**Decay poll**, 06:45:59Z -> 06:51:10Z, 11 polls, `decay-poll.log`:
**28 slot readings, every one LIVE**, `0 DEAD` in every summary. Last reading
is T+6m31s after the LEAVE job ended.

**The control earns the other readings their meaning.** Without it, "2 of 16
slots in use" is equally consistent with a registry that never empties.
`TSTAPPDC` called TERMAPI and its slot went away -- the count returned to
exactly the two leaked ones, not three.

**A design decision confirmed by accident.** Both LEAVE slots carry the same
ASCB with different token indices -- the "two INITAPIs from one address space
share an ASCB" case the kickoff gave as the reason the app *token*, not the
ASCB, stays the scoping key. Had TERMAPI been keyed on the ASCB, one
instance's teardown would have destroyed the other's sockets. That call is
now backed by a live reading. The generation counter showed too: slot 2 was
used by CLEAN as token `00010002` and reissued to HANG as `00020002`.

## Predictions

Recorded in `predictions.md` **before** the CLEAN and CANCEL arms ran.
P1 (CLEAN reports the same ASCB/ASID -> initiator reuse), P2 (CANCEL reads
LIVE, so it is not a second case), and P3 (no decay) all **fired as written**.

## What this round does NOT establish

- **The STC and TSO case.** An STC or a TSO user *is* its own address space
  and does terminate, which is the case the classifier was built for and the
  case that matters for M6 -- HTTPD and mvsMF are both STCs. It was **not
  measured**; it needs a client running as a started task and a start/stop
  cycle. Note that `TSTDEATH`'s DEAD rows do not cover it either: that test
  stages a **synthetic** dead identity (`tstd_free_asid` scans the ASVT for an
  entry that is *already* AVAILABLE). **No test in this tree has ever watched
  a real address space die.**
- **Whether any usable signal exists for the batch case.** Job termination
  clearly does not reach the ASVT. Whether some other control block records
  it, and whether it is reachable and trustworthy from an unauthorised STC, is
  unexamined.
- **Anything about issue #64.** Repeated MODIFY commands POST the cib ECB in
  the executive's own ECBLIST, so this round perturbed the executive
  deliberately and is not evidence about its idle behaviour either way.

## A separate finding: the rate limit has no clock

Independent of the above, and a footnote beside it -- it governs how often to
run a sweep whose premise has failed for batch clients.

The locked design is "at the head of the drain, at most once per 100 ticks
(~10 s), no timer". **There is no tick clock that can express that.** Since
ADR-0034 the invariant is *queue empty <=> STIMER disarmed*, so with no timer
armed `nsftmr_wake` is never called and no NSFTMR-derived tick count advances
at all -- 64-1 measured one executive pass in 259 s on an idle floorless
executive. A rate limiter reading such a clock finds `now - last == 0` and
never fires.

The failure case is not "nobody asks". It is **somebody asks after an idle
period**, which is the normal case for a stack whose applications come and go.
Decisions 1 and 2 are jointly unsatisfiable as written. Options, none chosen:

1. limit by executive passes instead of ticks -- honest to decision 1's own
   rationale ("a request is a pass"), but the unit is not time;
2. `nsf_now()` with a documented per-platform constant -- both platforms
   happen to give ~1 s per `hi` unit, but `nsftime.h` explicitly forbids
   deriving wall-clock from it, so this needs a decision, not a coincidence;
3. a new tick seam -- correct, and foundation work on ADR-0034 ground.

## Housekeeping

Each LEAVE / CANCEL arm costs one app slot of sixteen; this round leaked
three. `P NSFS` / `S NSFS` resets the registry -- verified, not assumed: the
recycle to STC 1505 was followed by `0 OF 16 SLOTS IN USE`, with no `NSF054W`
and `SVC 239 RESTORED`. The stand was left with NSFS running and the registry
empty.
