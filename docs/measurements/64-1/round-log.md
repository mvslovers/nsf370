# 64-1 live round -- raw log

Stand: MVSCE on mvsdev, 2026-08-27. MVS clock = UTC-5; host times CEST.
NOT IPLed this round (offered; not needed -- see "console repair").

## Console repair (an intervention, logged as one)

The master console was stuck in `*IEA420A NO FULL CAPABILITY CONSOLES,
REASON=EXT` -- residue of 64-0d's `ext` rung, which needs a SECOND press to
complete the switch and never got one. Symptom: commands entered at 0:0009
(REST or `/`-prefix) were ACCEPTED and QUEUED but never executed; `mvslog.txt`
(the 1403 hardcopy at CUU 0015) froze at 12:34. A second `ext` at 13:31 gave
`IEE143I CONSOLE SWITCH, REASON=EXT  OLD=009 NEW=009` and FLUSHED every queued
command at once (three `D T`s, a `D U`, an `S NSFS`). No IPL taken.

## Deployed binary at round start

64-0c's module (64-0d deployed nothing). Confirmed by FIELD, not message id:
`NSF812I ... WPREG=Y` present and `NSF813I BUSY=0 BUSYSLOT=-1` leading.
So the state-B arm below cost ZERO deploys.

## Gate 1

STC 1480 (reset ABSENT, no deploy), STC 1481 (reset PRESENT, deployed 13:45).

| # | state | POSTED | WAKEECB | EVTPASSES window | rate | WAKEPOSTS | host CPU us |
|---|---|---|---|---|---|---|---|
| B0 | 1480 fresh, no request | N | 009DCD10 | 958 -> 3544 / 259 s | 9.98/s | 0 | 0.5-0.9 % |
| B | 1480 after 8 requests | **Y** | 40000000 | 250992 -> 2188809 / 259 s | **7482/s** | latched, +3755 offset | **25.9 / 30.5 %** |
| A | 1481 after 8 requests | **N** | 00000000 | 718 -> 3303 / 259 s | **9.98/s** | **8** (= SERVED) | **0.7-1.6 %** |

Deploy-took-effect for this round: `POSTED=N` with `SERVED=8` (B is impossible
after a request). Deploy output clean, no mid-chain HTTP 500.
Local artifact sha256(build/NSFS) prefix eed1f714efa0e0c1.

SOLO runs: JOB02324 (B) CC 0000 8/8, JOB02326 (A) CC 0000 8/8 -- both complete
inside ONE console second, step CPU 0.08 / 0.11 s. Service is not degraded.

## Instrumentation load, logged

A first state-A CPU sample read 17-20 % us. Cause was MY OWN polling: a
stallwatch.py left running, reading 64 slots x /.dm every 2 s through HTTPD in
the guest. Stopped; the clean sample is 0.7-1.6 %.

## TSTRQXM (reset build)

JOB02327 batch CC 0000, **32/32 PASS**; host peer verified 9353 bytes
byte-exact, log mtime 13:55:10 fresh (64-0b's stale-log catch applied).
Includes CONNECT -- the first PARKED request to complete -- so the parked path
is exercised on the reset build.

## Floor measurement (state A, after the TCP workload) -- the headline

TSTRQXM ran (TCP connect/send/close), so the timer queue drained and ADR-0034's
invariant disarmed the STIMER. Then 259 s of idle:

    T0 13:57:13  EVTPASSES=5854  WAKEPOSTS=28 SERVED=28 TMRQ=0 POSTED=N
    T1 14:01:32  EVTPASSES=5855  WAKEPOSTS=28 SERVED=28 TMRQ=0 POSTED=N
    host CPU 0.2 % / 0.7 % us, 99.1-99.8 % idle

ONE pass in 259 seconds = 0.0039/s. There is NO floor: the heartbeat is gone,
exactly as ADR-0034 predicts, and the executive blocks.

PAIRED with service, which is what stops that reading meaning "dead":
JOB02328 submitted 14:02:54 on that same floorless instance -> 8 cross-AS
requests served and returned inside ONE console second (7.02.54 start AND end),
CC 0000, 8/8 PASS, EVTPASSES 5855 -> 5872 (17 passes for 8 requests).

So the wake works with no floor at all. "No floor is needed" is a measurement
here, not an assumption -- for the HEALTHY case. It says nothing about #64.

WAKEPOSTS == SERVED exactly at every reading (8/8, 28/28, 36/36): the counter
now counts wake events, one per request.

## Gate 1, all three revert states

| state | source | POSTED | EVTPASSES after 8 req | idle rate | host CPU us | SOLO |
|---|---|---|---|---|---|---|
| B  | reset absent (64-0c binary, no deploy) | Y | 250992 | 7482/s | 25.9-30.5 % | CC 0 8/8 |
| A  | reset present (deployed 13:45) | N | 510 | 9.98/s | 0.7-1.6 % | CC 0 8/8 |
| C  | reset commented OUT + deployed | Y | 52946 | 8532/s | 26.0-26.9 % | CC 0 8/8 |
| A' | restored + deployed | N | 340 | (see below) | (see below) | CC 0 8/8 |

State C's source was verified IDENTICAL TO `main` at instruction level (comment
-stripped diff empty), so the reset was provably out, not merely believed out.

Every deploy output was read for the mid-chain `HTTP 500` DELETE signature;
none occurred.

**The load-module SHA-256 is NOT a source fingerprint and is not used as
evidence.** Two builds of byte-identical source differ in exactly 2 bytes (a
build timestamp at offset 10560). Measured, not assumed.

## Restored arm (A')

    T0 14:10:30 EVTPASSES=698   POSTED=N WAKEPOSTS=8 SERVED=8
    T1 14:13:09 EVTPASSES=2286  POSTED=N WAKEPOSTS=8 SERVED=8
    159 s -> 9.99/s; host CPU 0.7 % / 0.5 % us

## Functional regression (final restored module)

make test-mvs --only TSTRQXM --only TSTRQXF --only TSTRQXC, MBTTEST JOB02334:
  TSTRQXC  batch CC 0 / TSO CC 0   8/8
  TSTRQXF  batch CC 0 / TSO CC 0   53/53
  TSTRQXM  batch CC 0 32/32 ; TSO FAIL CC 1 BY DESIGN (one-shot listener
           consumed by the batch run -- the 2 fails are CONNECT and its
           dependent CLOSE). Host peer verified 9353 bytes byte-exact,
           log mtime 14:16:06 fresh, listener confirmed in LISTEN before.
  172 PASS / 2 FAIL overall.

## Gate 2 campaign

Detector armed 14:17:06 on ASCB FF8F18 / anchor 00A8D7C8 (EJST flat AND a slot
PENDING AND served frozen for >= 20 s). No MODIFY used as a detector.

Rounds 1-15, back to back, each A+B submitted together:
  every round completed in ~10 s; 14 x A CC 0000, 1 x A CC 0001 (JOB02349).
  The CC 0001 is a GATE-INTERNAL timing assertion -- "A was given a slot other
  than 0", which the gate's own text calls the weaker witness and "NOT a second
  independent witness". Its decisive assertion (A REFUSED while slot 0 was the
  only free slot) PASSED, and collisions still moved by 149. Not an NSF defect.

After 15 rounds: SERVED=55475 EVTPASSES=110668 (about 2 passes per request),
COLLISIONS=270995, EXHAUSTED=4199, WAKEPOSTS=55384.

WAKEPOSTS (55384) is now slightly BELOW SERVED (55475) for the first time --
91 fewer. That is the documented coalescing: two posts arriving between one
observation and the next read as one. It confirms the counter is a LOWER BOUND
on posts, not a tally, which is what the declaration comment claims.

CAVEAT ON EXPOSURE: these rounds finish in 10 s each, where 64-0c/64-0d's gate
rounds were slow and disrupted (CC 20 / CC 1) and the stalls fired inside them.
15 x 10 s is LESS total exposure than a single 64-0d gate round, so "15 rounds,
no stall" is weaker than it sounds. Hence the sustained campaign below.

## Notes to fold into the record

- The 15 fast gate rounds are worth comparing to M5-2b4's own experience, but
  ONLY as an observation, not a controlled result: b4 recorded that its gate
  "crawled at ~3 requests/min until a continuous host `ping -i 0.2` was
  started (the #64 latency defect)". This round's rounds needed no external
  floor and ran 3000 attempts in ~10 s each. Different rounds, different stand
  state, no control -- suggestive, not evidence.
- Detector silent throughout (no false positives either -- it was armed across
  idle stretches where a naive EJST-flat detector would have fired constantly,
  since the floorless executive IS flat).

## Detector validation -- done BEFORE its silence was quoted as a result

stallwatch.py's whole conjunction rests on `req_state` read at
ANCHOR+0x38+i*2144. If the stride or offset were wrong it would read FREE
forever and report "quiet" straight through a stall -- an absence
indistinguishable from success (CLAUDE.md 8.5). Validated live, during the
campaign, with slotdump.py:

    14:29:16 census {'FREE': 49, 'CLAIMED': 15}
    14:29:23 census {'FREE': 64}
    14:29:28 census {'FREE': 63, 'HELD': 1}     <- SLOT63 HELD
    14:29:35 census {'FREE': 46, 'CLAIMED': 18}

and the PENDING conjunct specifically, by fast-sampling slots 0-3:

    360 reads -> {'FREE': 340, 'CLAIMED': 15, 'PENDING': 3, 'DONE': 2}

So all four states the detector distinguishes are readable through this exact
path. Its silence is a real null.

## Exposure, in the unit that matters

Every stall on record shared ONE condition: a client parked with a PUBLISHED
request. The measurement above puts a number on how much of this arm had it:
PENDING appeared in 3 of 360 samples on slots 0-3 = ~0.8 % of wall clock,
DURING the heaviest workload the stand can produce. In 64-0d a single gate
round held a client parked for MINUTES.

So the correct statement of the null is not "45 rounds, no stall" but "the
condition every stall shares was present for a few seconds in total across the
whole arm". That is a NAMEABLE weakness, not a generic one.
