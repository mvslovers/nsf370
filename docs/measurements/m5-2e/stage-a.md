# M5-2e stage a — the instrument

**Status: stage a complete. Stage b has NOT started, and two things must be
ruled on first (§6, §7).** No measurement run was made: every live figure below
is from a deliberately shortened TRIAL window, stamped as such by the program
itself, and none of it is a baseline.

Branch `m5-2e-exit-gate`. Host suite 3414 PASS / 0 FAIL, unchanged — and that
is a **no-regression check only**, evidence of nothing else, since the only
changed test is `host = false`.

---

## 1. What `TSTRQXC` could report unchanged — established from source

The kickoff names three things. None of them was present, and the reason in
each case is that the b4 roles were built to answer a yes/no question:

| asked for | b4 as it stood |
|---|---|
| progress at several points in the window | **absent** — `xc_run_a`/`xc_run_b` accumulate `ok`/`nobuf`/`bad` and print one total at the end. A client that starved for four minutes and caught up at teardown prints the same totals as one that ran evenly. |
| per-request delay | **absent** — the file reads no clock at all. `nsf_now` was not linked (`sources = ["test/mvs/tstrqxc.c"]`), and nothing timed anything. |
| a timed window, first minute excluded from the figures | **absent** — the loops are bounded by a REQUEST COUNT (`XC_N_PHASE1` 150, `XC_N_PHASE2` 3000, `XC_B_CAP` 20000), which makes the window length an output rather than an input. There is no notion of a start-up phase. |

What b4 *does* have and the measurement roles reuse unchanged: the anchor
chase, the request shape, the per-request identity check (`xc_request`'s
`mine`), the barrier, and the `XC_FN_UNKNOWN` verb — a function code no
dispatcher case claims, so it completes `NSF_EINVAL` with **no side effect in
the STC**. That last one is why this is the transport form Mike's decision 3
asks for: hundreds of thousands of requests leave no app slot and no socket
behind.

## 2. The extension — small, and built

Confined to one `host = false` test file plus one `project.toml` sources line:
`test/mvs/tstrqxc.c` +424 lines, `asm/nsftime.asm` added for `nsf_now` (the
TSTTMACC precedent; its `(b-a)>>12` µs conversion is lifted from
`test/mvs/tsttmacc.c`, where it was first checked against a known 100 ms
interval). The project.toml comment that read "Links only libc370" was true
before this change and is now false, so it was corrected in the same change.

Four new roles, all `static`, no new external symbol, alias scan **244 unique,
all ≤ 8** — unchanged, because a test change cannot add one:

- `MS` — one client, the reference arm. Without it "two clients are no faster
  than one" has no one-client number of its own to stand on.
- `MA` / `MB` — two address spaces, barriered at the start.
- `MSP` — `MS` with a deliberate pause **inside** the timed region: the
  discriminating gate on the instrument (§2.2).

`SOLO`, `A`, `B`, `LEAK`, `V` and `RESET` are untouched in behaviour, and the
splice is purely additive (the role parser returns 0 for anything not starting
`M`, so those six reach the same dispatch as before). `make test-mvs --only
TSTRQXC` runs `SOLO` and returned **CC 0 batch+TSO, 16 PASS / 0 FAIL** on the
final build — but that is **SOLO only** (8 assertions × batch+TSO). **`A`,
`B`, `LEAK` and `V` were NOT run this round**, so calling this "the b4
regression" would overstate it: the two-client gate and the retain-branch
induction are unexercised on this build.

### 2.1 The discard is a checkpoint, not a reset

The first `XC_DISCARD_S` (60) seconds are measured like any other and reported
**separately**: `_all` covers the whole window, `_rep` starts at the crossing,
and both are printed. So the discard is auditable rather than asserted, and
the boundary is a compile-time constant that cannot be chosen after seeing the
data. A trailing decimal on the PARM shortens the window and **stamps the run
a TRIAL**; it never scales the discard, and a plain `MS`/`MA`/`MB` is always
the full window — so a measurement run cannot inherit a trial's shape by
accident.

### 2.2 The instrument's own gate — and the bug it caught, which was mine

A latency figure that is simply wrong looks exactly like a fast transport, and
a histogram whose samples all land in bucket 0 looks exactly like a
well-behaved one. `MSP` pauses ~10 ms inside the timed region and asserts min,
mean **and that every bucket below 5 ms is EMPTY** — min/max alone would pass
with the bucketing broken.

**The first `MSP90` run reported `13/13`… no: it reported 8/8 and had tested
nothing.** `xc_run_measure` keyed the pace off `role[1]`, which is `'S'` in
`"MSP"`, so the pace never armed, `xc_assert_paced` never ran, and the job
returned **CC 0** having measured an ordinary unpaced run. Caught by reading
the output, not by the return code — CLAUDE.md §8.5 inside the safeguard built
to enforce it. Fixed three ways, because one was not enough: one flag now
drives both the arming and the assertion; the assertion itself checks the arm
(`CHECK(g_pace != 0u, ...)`); and the pace is **printed in the report header
whether or not it is armed**, so an un-armed paced arm is visible even to a
reader who never looks at the assertion list. Evidence of both states is kept:
`trial-msp90-selftest-DID-NOT-RUN.txt` and `trial-msp90-selftest-fixed.txt`.

### 2.3 Offline gates

- Cross-build clean: **6 modules + 53 test modules** (cc370/as370/ld370).
- **`[build].cflags` carries no warning flags** (project.toml:8) — `-Wall
  -Wextra -Werror` live only in `[host].cflags` (:14). So a `host = false`
  test gets **no warning coverage on either build**, which is a standing
  property of this project and not something (e) introduced. TSTRQXC was
  therefore built once with `-Wall -Wextra` added to `[build].cflags`
  temporarily: **clean**, and **verified to discriminate** — an injected
  unused variable produced `tstrqxc.c:900: warning: unused variable`, so the
  clean run is evidence rather than an absence of complaint. Flags reverted.
- The checkpoint grid advances to the next grid point **past** the elapsed
  time rather than by a fixed increment. A request that stalls across two
  boundaries would otherwise leave the next target behind the clock, fire
  again immediately, and report an interval that is not `XC_CHK_S` — a
  spuriously short bucket in the one table stage b relies on, and under #64 a
  multi-second stall is not hypothetical. A skipped grid point now shows as a
  `t_ms` column that jumps by two intervals, which is visible.
- A trap worth recording: **GNU Make 3.81 compares mtimes at one-second
  granularity**, so an edit-build-revert cycle inside one second leaves the
  previous module in `build/` and reports "Nothing to be done". Every build
  above forced the object out first.

## 3. The trial — six live runs, all TRIAL shape

Stand: MVSCE on `mvsdev`, `NSFS` STC started clean on a machine with no CSA
debt (`NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144) -- LARGEST FREE BLOCK
NOW 1073152`), `NSF851I NSFS NON-SWAPPABLE (NDS=1)`, devices up at MTU 1500,
`NSF042I SVC 239 STOLEN (EP 00A82108)`. Client **UNAUTHORISED** in every run
(`TESTAUTH FCTN=1 == 0`, asserted). 90 s window, 60 s discard, **30 s
reported** — these are not baselines and are not comparable with anything
stage b produces.

| arm | job | served/s | mean | min | max | refused | bad |
|---|---|---|---|---|---|---|---|
| `MS` solo, no ping | JOB02950 | 1893 | 521 µs | 242 µs | 2972 µs | 0 | 0 |
| `MS` solo, host ping | JOB02951 | 1959 | 503 µs | 238 µs | 2817 µs | 0 | 0 |
| `MA` two-client | JOB02948 | 2299 | 428 µs | 245 µs | 3959 µs | 0 | 0 |
| `MB` two-client | JOB02949 | 2296 | 428 µs | 235 µs | 3765 µs | 0 | 0 |
| `MSP` paced 10 ms | JOB02947 | 93 | 10689 µs | 10482 µs | 13155 µs | 0 | 0 |
| `MS` solo, **committed build** | JOB02954 | 1888 | 522 µs | 256 µs | 2710 µs | 0 | 0 |

The last row exists because the checkpoint-grid fix (§2.3) landed **after** the
first five runs, which would otherwise have left every trial figure describing
a build that is not the committed one — the cross-build comparison this round
refused to make elsewhere. Re-run on the committed build: **1888 vs 1893
served/s, mean 522 vs 521 µs**, checkpoint intervals still 15 s, 8/8. The fix
changes nothing outside the stalled-request case, which is what it was for.

All CC 0000. `bad = 0` across **421 208** requests in the two-client trial
alone — the per-request identity check holds at a scale b4 never reached.

**The instrument is validated by the pace moving the distribution 21×** —
whole-window and on the SAME build, `MS90` 1958 served/s at min 238 µs against
`MSP90` 93 served/s at min 10 423 µs. That is the criterion: had the timing
been broken, the injected pause would not have shown. (The comparison is
deliberately not made against the first, pre-fix `MSP90` run: that build had
different bucket boundaries, and a cross-build comparison is the habit this
round exists to avoid.)

**Progress sampling works and shows no starvation.** Per-15 s deltas, MA:
35 749 / 34 856 / 35 661 / 35 564 / 35 658 / 33 320; MB: 35 753 / 34 681 /
35 620 / 35 447 / 35 681 / 33 216. The two clients are within 1 % of each
other in every interval. (The first checkpoint fires at k=0 and so records
`served=1` at t≈1 ms; the per-interval deltas after it are correct.)

**The barrier skew is measured, not assumed.** Both jobs print opening and
closing TOD and both read one hardware clock, so they subtract exactly: MB
opened **270.4 ms** before MA and closed 270.2 ms before it, each window
90.000 s, **overlap 89.730 s = 99.70 %**. At the 300 s stage-b window the same
skew is 99.91 %.

### 3.1 Three findings from the trial that shape stage b

**(a) `refused` is ZERO in the measurement shape, and that is structural.**
With 64 slots and two clients, no client is ever turned away — b4's refusals
came from *deliberately pre-claiming 61 slots*, which is a gate, not a
baseline. So the deferral evidence Mike's decision 5 asks for lives **entirely
in the served-latency distribution**, and the refusal histogram will be empty.

**The STC corroborates this from its own side**, which matters because the
clients' `refused = 0` is only their own count: `F NSFS,STATS` after the round
reads `EXHAUSTED=0` — nobody was ever turned away — beside
`COLLISIONS=208971`. So the contention was **real and slot-level** (a claim
scan repeatedly found a slot that was not FREE, ~0.5 times per request across
the 421 208 two-client requests) and yet **never reached exhaustion**. Two
different facts that a single client cannot produce and that the served/refused
split alone would have conflated: contention here is service serialisation,
not slot starvation. (`BUSY=0 INFLIGHT=0 REAPED=0`, and `NSF817I APPSWEEP
SWEEPS=111 RECLAIMED=0` — the side-effect-free verb left no app slot behind,
as intended.)

**(b) The serialisation prediction is not what the trial saw, and stage b must
quote it as written anyway.** Two clients delivered **4595 served/s combined**
against ~1900 for one — about 2.4×, and per-client mean latency went *down*
(521 → 428 µs). The plausible reading is that one client leaves the executive
idle between its own requests, so the second fills gaps rather than queueing
behind service; but this is a 30 s trial, not a measurement, and it is
recorded as a thing to test, **not as a result**.

**(c) The bucket boundaries were resized from the trial and are now fixed.**
The first set put 175 249 of 175 343 samples into two buckets. The current set
— 100 / 250 / 500 / 750 / 1000 / 1500 / 2500 / 5000 / 10 000 / 50 000 /
250 000 / 1 000 000 µs — spreads the 250–1000 µs mass across four buckets
while keeping the tail. Min/max/sum are carried alongside, so a boundary set
that turns out wrong under two-client load costs resolution, not the reading.

## 4. A validity condition the kickoff does not list — measured and fixed

§3 lists swap, the counter cutoff, `wakeposts` and a clean machine. It does
not mention **the wake floor**, and it should: M5-2b4's gate crawled at
~3 requests/min until a continuous host ping was started, and #64 is open and
mitigated-not-fixed. A latency distribution taken without controlling for this
measures #64, not the concurrency model.

Measured rather than argued: `MS90` twice on the same build and instance, one
arm with a continuous `ping -i 0.2` from the host (verified on the wire, NSF
answering), one without. **1893 vs 1959 served/s; mean 521 vs 503 µs** — a
3.5 % difference, in the direction of noise. A saturating client is its own
wake source, so **stage b runs with NO external wake floor**, and that is now
part of the baseline's definition: a later comparison run under the other
condition is not comparable with it. The ping was stopped and **its absence
confirmed by `ps`, not by `pgrep`** — `pgrep -f` matches the ssh command line
that carries the pattern and reported a survivor that did not exist.

## 5. What stage a does NOT establish

- Nothing about throughput or latency as a **baseline**. Every figure here is
  a 30 s reported region inside a trial-stamped 90 s window.
- Nothing about a 300 s window, three valid runs, or run-to-run spread.
- Nothing about swap behaviour during a measurement window (no `OUCBQFL` /
  `ASCBSTOR` / `OUCBSWC` sampling was run — that is stage b's §3).
- Nothing about the exit gate's three properties (§6).
- The two-client throughput reading is from ONE 30 s region of ONE pair of
  jobs. It is a reason to predict carefully, not a measurement.

## 6. BLOCKING — §1.2's first half is a property the code does not have

> "A cannot use B's socket descriptors — a verb on a foreign descriptor is
> refused"

**Measured from source, by exhaustive enumeration: there is no such check.**
`req_socket()` (src/nsfreq.c:544-547) is `sock_lookup(r->sockdesc)` and
nothing else; `sock_lookup` (src/nsfsoc.c:104-121) validates the table index
and the slot generation, and returns the socket. `apptok` appears **28 times**
in all of `src/` and `include/`, and the only place a socket's `apptok` is
compared against a request's token is `term_one` (src/nsfreq.c:406) — the
TERMAPI mass-teardown callback. Not one per-verb dispatch path consults it.

It is two levels, and they differ:

- **At the EZASOKET facade the property effectively holds.** A client names
  its sockets by a halfword index into its own per-app table, in its own
  address space; it cannot *name* another client's socket through the API.
- **At the raw NSFRQE level it does not.** A client that builds its own
  request — exactly what `TSTRQXC` does, from an **unauthorised** batch job —
  puts any `sockdesc` it likes in the image, and the dispatcher honours it.

**And the value is not merely forgeable, it is guessable, because the socket
table is one table.** `g_socktab[NSFSOC_MAX_DEFAULT]` is a single static array
and `sock_alloc` (src/nsfsoc.c:125-157) scans it for the first free index
*regardless of which app is asking*, so A's socket and B's socket are
neighbouring entries in one space. The descriptor is `(gen<<16)|idx` with
`idx` in 0..63 and `gen` the slot generation, which starts at 0 on a fresh STC
and only advances when that slot is reused — so on a freshly started NSFS the
first descriptors handed out are small integers, and a client can calibrate
against its own. This is the difference between "no check exists" and "no
check exists and the value falls in tens of attempts", and it is the half that
should decide the ruling.

**The adjacent `apptok` case is weaker than it first looks, and the guard is
real.** `nsfreqx_dispatch_in` rewrites only `ecb`, `ubuf` and `ulen`, so the
client-supplied `apptok` does cross unchecked — but `do_termapi` goes through
`app_index` (src/nsfreq.c:260-272), which rejects any token whose index is out
of range, whose slot is not `inuse`, or whose generation does not match, and
`do_termapi` completes `NSF_EINVAL` on that (:414-419). So an **arbitrary word
is refused**; what is missing is any binding between the *caller* and the
token, so a client holding or guessing a **live** token can use it. The
descriptor case has no such guard at all. An earlier draft of this section said
a forged token "reaches the teardown path" without having read `app_index` —
it does not, and the corrected claim is narrower.

This is the category already filed twice in this tree — M5-2c0's "an
unauthorised caller can dispatch a probe verb at NSFS", and the b1 follow-up's
"removing the probe scaffolding is a SECURITY item, not only hygiene".

**§1.2's second half is different and appears sound.** "A's TERMAPI or death
leaves B's sockets and app slot untouched" is scoped by token at
src/nsfreq.c:406 and by `app_index` at :414, so for an *honest* A it should
hold — and it is worth testing, because whether it holds with two live clients
is exactly the open question.

**This blocks writing the gate sentence, which is why it is reported rather
than worked around.** The sentence was not rewritten to the facade level so it
would pass, and no gate was built that goes red and reads as a broken build.
**Mike's call:** does (e) assert the transport property — in which case the
gate records a finding, not a pass — or the facade contract, with the
transport gap filed separately?

## 7. Job A's instrument does not exist, and Job A is the exit gate

Stage a was scoped to `TSTRQXC` as a measurement instrument, and `TSTRQXC` is
the wrong shape for Job A by design: its verb is deliberately side-effect-free,
so it creates no sockets and no app slots, and its requests carry
`ubuf = NULL`. §1.1's progress evidence is now available; §1.2 and §1.3 are
not, and are not an extension of this file. Naming it plainly: **this is a
scheduling question, not a scope reduction** — Job A *is* the exit gate.

The proposed shape is a new TSTRQXM-sized two-client file (`RQXCM*`-style JCL
already exists as the concurrency mechanism):

- **§1.1** — the checkpoint machinery built here, reused.
- **§1.2** — INITAPI + socket + bind in each client; a verb on the other's
  descriptor; then A TERMAPIs and B's sockets and app slot are re-verified.
  Blocked on §6.
- **§1.3 is much cheaper than a TCP echo harness**, and that is the useful
  finding here. The reply copies the **same `xlen` bytes back out** into the
  caller's buffer (`src/nsfsx.c:1202`, and 80-FIX made the copy unconditional
  in **both** directions, `src/nsfsx.c:1344` being the copy-in), which is
  precisely what TSTRQXM's `pat_ok` already asserts
  (test/mvs/tstrqxm.c:93-105). So: two clients, each `sendto` a per-client
  fill pattern — seeded on `0xA1` and `0xB2`, so a foreign byte is
  recognisable *as* foreign — each verifying after **every** call that its own
  buffer still holds its own **full** pattern. **No echo peer is needed**;
  TSTRQXM's `PEER_PORT 9999` precedent ("nothing listens: we want the SEND")
  applies. Both `g_land` copies are exercised.

Worth stating for whoever writes it: under the current serialisation `g_busy`
is held from dispatch to completion, so `g_land` cannot be entered by a second
request between one request's copy-in and copy-out. §1.3 should therefore
**pass**, and Mike's instruction to prove it *positively* is the right shape —
it pins a property that today rests on an invariant one function away.

## 8. Red lines

Held, and by construction rather than by care: the change touches
`test/mvs/tstrqxc.c`, `project.toml` (one sources line + comments) and four
new `jcl/RQXCM*.jcl` files. **No module source changed** (`git diff
--name-only main` matches nothing under `src/`, `asm/`, `include/`,
`samples/`), so `asm/nsfvsvc.asm` is untouched, the anchor layout is unmoved,
`ANCVERNO` is 3 and `NSFRQE` is frozen at 64 B.

**`NSF.LINKLIB` needed no redeploy *for this change*** — no module source
changed on this branch. What is deployed is whatever the previous round left,
and that was **not verified against `main` this round**; the trial figures are
about that module, not provably about `main`'s. Stage b, which produces
numbers people will cite, must establish the deployed module positively rather
than inherit this sentence.
No target was invented; no counter reading predates `58dfaab`; nothing was
merged and no milestone flipped.

## 9. What stage b needs before it can start

1. **§6** — Mike's ruling on the §1.2 gate sentence.
2. **§7** — Job A is a new file. Build it, or rule that (e) runs Job B first
   and Job A follows.

Stand left clean: `NSFS` running, pool 64/64 FREE, no `NSF054W`, no dumps, no
ping processes, TESTLIB holding `TSTRQXC` alone.
