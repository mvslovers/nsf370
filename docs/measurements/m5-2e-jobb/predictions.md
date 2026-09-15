# M5-2e Job B -- predictions, written BEFORE any run

Written 2026-09-15, after the host reboot and before `S NSFS`. **Not edited
after the first run.** This is the third time in this project that a written
prediction has been made to earn its keep, and the previous one was **falsified
this morning** (`docs/measurements/ctci-tun-eintr/`), which is the argument for
writing them rather than the argument against.

---

## The environment IS a condition, because it is not reproducible from the numbers

| | |
|---|---|
| host | rebooted 2026-09-15 ~09:49; uptime minutes, not weeks |
| emulators running | **ONE** -- `MVSCE-DEV` only. The other four instances are deliberately not started. |
| wire | `tun0` present and up (`HHC00901I 0:0500 CTCI: Interface tun0, type TUN opened`) |
| MVS | JES2, NET, TSO, HTTPD; NSFS not yet started |
| modules | `NSF.LINKLIB` and TESTLIB survived the reboot on DASD; **no module source has changed since Job A's deploy** (verified: no commit since touches `src/`, `asm/`, `include/`, `samples/`) |

**A later reader comparing against this baseline needs to know the box was
carrying ONE emulator and not five.** Stage a's trial figures were taken on a
box that was, at that time, running the old single `~/MVSCE`; the five-instance
configuration came later. **No figure from stage a is comparable with anything
here** -- different window (90 s vs 300 s), trial-stamped, different host load.

**Counter comparability:** any `F NSFS,STATS` reading from before `58dfaab` is
not comparable with one after. (e)'s numbers start at the post-fix set.

**Instrument note:** the `zowe` CLI is broken under this machine's Node 26
(`util.isNullOrUndefined` was removed; even `zowe --version` throws inside
zowe's own error formatter, which hides the real error). It is driven through a
Node 22 wrapper for this round. `make deploy` / `make test-mvs` are unaffected
-- mbt speaks to mvsMF over Python urllib, not through zowe.

---

## P0 -- the pool reads 1073152

`S NSFS` reports `NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144) -- LARGEST
FREE BLOCK NOW 1073152`.

**A retained anchor is impossible after an IPL**, so a lower value cannot be
CSA debt from a previous run. **Falsified by any other value -- and the
response is to STOP and report, not to proceed and not to adjust**, because a
different value means something changed that this round has not accounted for.

*Clause checked:* the failure mode to avoid is reading a different number and
rationalising it. There is no value other than 1073152 that this round is
entitled to treat as normal.

## P1 -- the instrument gate (MSP) passes before any number is believed

`MSP` pauses ~10 ms inside the timed region and asserts min >= 9 ms, mean
>= 9 ms, and **every bucket below 5 ms EMPTY**.

**Predicted: passes.** **Falsified if** any sample lands below 5 ms, or if the
pace is reported un-armed.

*Clause checked:* a green `MSP` is only meaningful if the pace was actually
armed -- stage a's first `MSP` run reported 8/8 having tested nothing. The
report header prints the pace whether or not it is armed, and the assertion
checks the arm. **If `MSP` fails, no measurement number from this round is
believed**, and that is the point of running it first.

## P2 -- both clients progress in every interval, and neither starves

Both `MA` and `MB` report more than one checkpoint and a non-zero delta in
every interval after the first, tracking each other closely.

**Falsified if** either shows a zero-progress interval while the other
progresses. *Clause checked:* guarded against the two false alarms -- an
unsampled window (its own `g_nchk > 1` assertion) and a client that ended
early (impossible; one clock bounds the window and stamps the checkpoints).

## P3 -- two clients are NOT twice one, and the trial's 2.4x is quoted as a thing to test

Service is **serialised**: ADR-0042 §10 permits exactly one request in flight,
and concurrent service is a named open item (ADR-0043 gap (b)).

**Stage a's trial saw the opposite of naive serialisation** -- about 2.4x
combined against one client, with per-client mean latency going *down*. Stage a
recorded that explicitly as *"a thing to test, NOT as a result"*, and it is
quoted here as written rather than adopted.

**Predicted: combined two-client throughput exceeds solo, and is less than
2x solo.** The reasoning offered for the trial's result -- that a single client
leaves the executive idle between its own requests, so a second fills gaps
rather than queueing behind service -- would, if correct, allow a ratio well
above 1 and below 2.

**Falsified if** combined >= 2x solo (the serialisation model is wrong, and
that is a finding about ADR-0042 §10, not about this test), **or** if combined
<= solo (the gap-filling reading is wrong). **Both falsifications are
informative and neither is a defect in the instrument.**

*Clause checked:* the trap here is treating a ratio above 1 as surprising. It
is not -- serialised *service* does not imply serialised *arrival*, and the
executive is idle between one client's requests. What would be surprising is
2x or better.

## P4 -- no swap transition during any run

NSFS is non-swappable (`SYSEVENT DONTSWAP`, ADR-0044), so `OUCBQFL`,
`ASCBSTOR` and `OUCBSWC` should be constant across every run.

**A run in which a transition appears is DISCARDED, not corrected** -- and the
transition is **separately a finding about ADR-0044**, because a pinned address
space is not supposed to swap at all.

*Clause checked:* `ASCBSTOR` changing is the retrospective signature of a
*completed* cycle (64-0f), so it must be sampled as well as `OUCBQFL` -- a
fast out-and-back would leave `QFL` looking clean.

## P5 -- the stand is left as found

`LARGEST FREE BLOCK` equal before and after every run; **no `NSF054W`**; pool
64/64 FREE; zero dumps.

**An `NSF054W` DISCARDS that run** rather than footnoting it. An IPL is
available on demand, so a discarded run is a repeat, not a blocked round.

---

## What Job B will NOT establish, said before the runs

- **Anything about Job A.** Job A is merged (#108) and judged separately. **No
  timing from Job A is mixed in**, and Job A reported none.
- **Concurrent service.** The model is unchanged and serial; this measures it,
  it does not alter it.
- **Comparability with stage a.** Every stage-a figure is trial-shaped (90 s,
  trial-stamped) and taken under a different host configuration.
- **Anything about the CTCI failure.** The wire is up because the running state
  is fresh; that is recorded in the CTCI document and is not this round's
  subject.
- **A milestone flip.** Nothing here flips to proven; the M5-2 flip is Mike's.
