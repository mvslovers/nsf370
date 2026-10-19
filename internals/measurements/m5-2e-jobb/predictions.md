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

---

# ADDED 2026-09-15, AFTER the P0 stop and BEFORE starting anything

**Appended; nothing above edited.** The original predictions stand as written —
P0 among them, and **P0 was falsified** (`NSF055I` read 1187840). This block is
written before `UFSD` and `FTPD` are started and before the re-read.

## The "before" set, read rather than reconstructed

Taken at 03.14.57 MVS time, nothing started yet
(`task-set-before.txt`):

| task | state | recorded normal set |
|---|---|---|
| JES2 | UP | up |
| NET | UP | up |
| TSO | UP | up |
| **UFSD** | **not up** | up |
| HTTPD | UP | up |
| **FTPD** | **not up** | up |
| NSFS | not up | stopped |

**All six checked, not only the two.** The difference is **exactly** `UFSD` and
`FTPD` — there is no third missing task, so there is nothing further to report
and nothing beyond those two will be started.

## PB0 — the baseline prediction

> **With `UFSD` and `FTPD` started and the rest of the recorded set up, a fresh
> `S NSFS` will report `NSF055I ... LARGEST FREE BLOCK NOW 1073152` —
> EXACTLY, not approximately.**
>
> **Falsified by any other value, including higher and including close.**

**If it reads 1073152:** the 112 KB candidate is **confirmed by prediction
rather than asserted**, which is worth more than the arithmetic that suggested
it — the arithmetic only established that two absent STCs were of the right
order, not that they were the cause. Job B proceeds.

**If it reads anything else:** that is a **second finding**. Nothing further
will be started, stopped or restarted to make the number move. The value and
what was running when it was taken get recorded, and the round stops again.
**Adjusting until the figure matches would undo exactly what the P0 stop got
right this morning.**

*Clause checked:* the tempting error here is treating "close" as confirmation.
A value near 1073152 would mean the two STCs account for most but not all of
the difference, which is a *different* claim from the one being tested and
would leave an unexplained remainder. The prediction is an equality on purpose.

---

# ADDED 2026-09-15, AFTER the ruling and BEFORE any run

**Appended; nothing above edited.** PB0 stands as written and stands
**falsified** — that is not revised.

## The ruling, with its substance, because it is a decision and not a drift

**Proceed.** The equality was **overspecified**, for two reasons that are
measurements rather than interpretations:

1. **The marker's purpose is "no retained anchors going in" ((e) §5.4), and a
   retained anchor is a MEASURED quantity: 139264 bytes** — pool plus router,
   from the Stage 2 round. Today's difference is **4096 bytes, one quantisation
   step. An anchor would be 34 steps.** The condition's purpose is satisfied by
   a factor of 34. That is an order-of-magnitude comparison, not a judgement.
2. **`LARGEST FREE BLOCK` measures the largest CONTIGUOUS block — a
   fragmentation quantity, not an occupancy total** — so it depends on the
   order in which storage was obtained and released. **That order is
   demonstrably different today:** host reboot, `HTTPD` started by hand
   immediately after the IPL so the system could be reached, `UFSD` and `FTPD`
   started roughly nine minutes later. The equality was a proxy that held only
   while the start order was constant.

**THE RESIDUE IS RECORDED AS RESIDUE: we do not know what the ≤ 8 KB is.** The
round proceeds because the condition the marker protects is satisfied with room
to spare — **not because the discrepancy is explained.** "Resolved" would be
false.

The instrument characterisation stands as written: `nsfsx_csa_largest` doubles
until `getmain` fails, refines `while (hi - lo > 4096U)` and returns `lo`, so
1069056 and 1073152 are adjacent steps and one step apart means anywhere from
1 to 8191 bytes. **It does not confirm PB0.**

## The entry condition, restated by purpose (replaces the equality for this round)

- **Entry:** the reading is within a few quantisation steps of the clean
  expectation — no retained anchor, which would sit orders of magnitude above.
  **1069056 satisfies this.**
- **Per run:** the before/after delta remains the real retention test, and an
  `NSF054W` **discards** that run rather than footnoting it. Unchanged.
- **To record:** the value, the start order, and **explicitly that this
  baseline was taken under a different start order than the 1073152 series**
  (`40-chk/`, `40-ident/`, `stage-a.md`).

## PB1 — the 4 KB gets a TEST, not a shrug

> **If the per-run `LARGEST FREE BLOCK` delta is ZERO across all three runs,
> the 4096-byte offset is ESTABLISHED as a start-order artifact rather than
> assumed.**
>
> **Falsified if** any run shows a non-zero delta — which would mean the pool
> reading moves within a round, and the offset is then not merely a
> fragmentation artifact of the start order but something that also varies
> under load. That would be a finding in its own right.

*Clause checked:* a zero delta does **not** tell us what the 8 KB-or-less
is; it tells us the reading is **stable within a round**, which is what makes
"start-order artifact" the remaining explanation rather than one of several.
The residue stays residue either way.

## PB2 — the instrument gate (MSP) passes before any number is believed

Unchanged from P1 above, restated because it now runs at the 300 s window
rather than a trial shape: min ≥ 9 ms, mean ≥ 9 ms, **every bucket below 5 ms
empty**, and the pace reported ARMED.

**If MSP fails, no measurement number from this round is believed.**

## PB3 — swap: no transition in any run

`OUCBQFL` constant, **`ASCBSTOR` constant**, `OUCBSWC` constant, `NSW` set
throughout (NSFS is `DONTSWAP`-pinned, ADR-0044).

**A run with a transition is DISCARDED, not corrected**, and the transition is
**separately a finding about ADR-0044**.

*Clause checked:* `ASCBSTOR` is sampled because `QFL` alone cannot carry it — a
fast out-and-back leaves `QFL` looking clean, and a completed cycle shows
retrospectively in `ASCBSTOR`/`OUCBSWC`. **Sampling is at 30 s, not the
original 3 s, because the sampler reads through HTTPD on the guest being
measured** and this round measures throughput; the coarse interval still
detects a completed cycle and only loses mid-flight capture.

## PB4 — the shape of the numbers (unchanged from P2/P3 above)

Both clients progress in every interval; combined two-client throughput
**exceeds solo and is less than 2×** solo. Stage a's trial saw ~2.4× and
recorded it explicitly as *a thing to test, not a result*; it is quoted, not
adopted. **Falsified** by ≥ 2× (a finding about ADR-0042 §10) or by ≤ solo.
