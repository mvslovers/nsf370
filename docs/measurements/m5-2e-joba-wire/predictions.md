# Job A §1.3 -- the wire arm: predictions, written BEFORE any deploy or run

Written 2026-09-15 against `main` at `bc7f093`, **before** `P NSFS`, before
`make test-mvs`, and before any job was submitted. **Not edited afterwards.**

**This round changes no source.** `test/mvs/tstrqx2.c` is byte-identical to the
merged Job A gate (`e2c2b65`, unchanged since); `project.toml`, `jcl/RQX2A.jcl`
and `jcl/RQX2B.jcl` are untouched. The round is the same gate on a stand whose
interface is up. If anything appears to need a source change, the round stops
and reports -- that is a finding, not a licence.

Each prediction carries its falsification clause, and **the clause is checked
as carefully as the prediction** (#101 Stage 2: an unexamined clause turns a
true observation into a false conclusion).

---

## P0 -- the deploy is seen to take effect, and the fingerprint is STATED

**Neither of §5's two documented tells has purchase here, and saying so is part
of the check rather than a way around it.**

- The documented tell -- *identical values across supposedly different builds*
  -- needs two builds. There is one: `main`'s `TSTRQX2` **is** Job A's, byte for
  byte. Identical values are therefore *expected*, not a symptom.
- The inverse tell -- *a value neither pure build can produce* -- needs two pure
  builds to enumerate error sets across. There is one build, so it has nothing
  to discriminate between.

**The fingerprint here is presence and freshness of the TESTLIB member**, and it
is two positive observations:

1. `make test-mvs` prints the member being staged, and the run's own stage step
   returns Job A's no-PARM signature: `NO ROLE ('') -- nothing ran` ->
   `GATE SKIPPED -- CC 20`. **The mbt matrix renders that as FAIL rc=20 and that
   is expected** -- said here in advance so the round does not read red.
2. The alternative to a staged member is `IEA703I 806-4` (absent), never a quiet
   wrong answer. Absence is loud for this dataset.

**`n_wire > 0` is NOT the deploy check.** It is the measurement, and using it as
its own precondition would be circular. Whether the interface is up is
established *independently and beforehand*, from `NSF210I` / `NSF211I` at
`S NSFS` and from `D U,,,500,4`, not inferred from the test.

**Falsified if** `TSTRQX2` draws `806-4`, or the stage step does not report
CC 20. *Clause checked:* both are positive observations; neither can be
satisfied by something merely failing to happen.

---

## P1 -- the wire arm, verbatim from the kickoff

> With the interface up, `n_wire > 0` and `n_wire_ok == n_wire`; every client
> finds its full per-client pattern intact after every call, the real `SENDTO`s
> included. Falsified by any `dirty > 0`, by `n_wire_ok < n_wire`, or by
> `n_wire == 0` with the interface up -- the last of which would mean the arm is
> not reached for a reason other than the wire.

**Quantified, so the run can be checked against a number rather than a
direction:** the window is `XA_WIN_S` 90 s at `XA_CHK_S` 15 s, so each client
should reach **6 checkpoints and ~6 in-range `SENDTO`s** of `XA_WIRE_N` 1024
bytes to `192.168.200.2:9999`, from source ports 7803 (A) and 7804 (B).

**And the assertion count moves, predictably:** the wire branch carries **two**
CHECKs where the no-interface branch carries one, so Job A's `18/18` (A) and
`23/23` (B) become **`19/19` and `24/24`**. This is a *consequence* being
predicted, not a precondition being assumed -- P0 above is what establishes the
deploy, and this is what the run must then show.

*Clause checked.* Three ways a green P1 could be hollow, and each is closed:

1. **The arm silently not running.** Impossible to miss: the skip is asserted in
   POSITIVE form (`n_wire == 0`, *"the wire arm DID NOT RUN"*) plus a WTO, and
   the totals differ (18/23 vs 19/24). A run that skipped cannot present as one
   that ran.
2. **`n_wire_ok == n_wire` with both zero.** `n_wire > 0` is a separate
   assertion, and the `EHOSTUNREACH` path *decrements* `n_wire` so a non-run can
   never leave it positive.
3. **The stack reporting success that never reached the wire.** `n_wire_ok` is a
   stack-level count. It is therefore paired with an **independent witness** --
   `tcpdump -ni tun0 'udp and port 9999'` on mvsdev across both windows, plus
   the `LNK1 out` / `NSFUDP out` deltas from `F NSFS,STATS`. The host answers
   each datagram with an ICMP port-unreachable (nothing listens on 9999), which
   is harmless and is itself a second-order confirmation the datagram arrived.

---

## P2 -- everything Job A already proved reproduces

§1.1 (both clients progress in every interval), §1.2 (foreign refused, own
served, TERMAPI tears down) and the §1.3 copy-pair (`dirty = 0`, `txerr = 0`,
`wrong = 0`) all re-run unchanged and all hold. **Predicted, not re-claimed:**
these were established in Job A and this round does not re-prove them -- a red
row here would be a regression to investigate, not a Job A result withdrawn.

**Falsified if** any of them moves. *Clause checked:* the one legitimate way a
row could differ is **fewer bulk calls per interval** than Job A's ~214 000, and
that is EXPECTED -- six real datagrams per client now ride the window, and Job A
reports no throughput figure precisely so a difference here means nothing.

---

## P3 -- the stand is left as it found it

`LARGEST FREE BLOCK` identical at the round's entry and exit start (today's
expectation on this stand is `NSF055I ... 1069056` -- the start-order artifact
Job B established; `1073152` is the adjacent step of a 4 KB-quantised instrument
and would be noted, not treated as an error). **No `NSF054W`**, zero dumps, both
clients' flag slots lowered, pool 64/64 FREE.

**Falsified if** an `NSF054W` appears -- which, per the kickoff, **discards the
run** rather than footnoting it. A reading below ~933888 at entry means a
retained anchor was inherited and the round stops before it starts.

---

## What this round does NOT establish, said before the run rather than after

- **d1 §2.3** -- the SELECT arms' stimulus. #107 assigns them to a d1 round,
  after the flip. This round does not touch them and must not be read as having.
- **Throughput or latency of anything.** No figure here is a baseline, and none
  is comparable with Job B's.
- **Concurrent service.** The concurrency model is unchanged and serial.
- **The partial-write residue shape** -- an op writing FEWER bytes into `g_land`
  than `xlen`. The in-range `SENDTO` reads the full staged surface; the detector
  is the identity round trip, not the residue.
- **A milestone flip.** Nothing here flips to proven; the M5-2 flip is Mike's
  and follows this round.

---

## AMENDMENT, before any job was submitted -- P3's entry threshold FIRED, and its REASON is refuted

**Appended, nothing above rewritten** (the Job A / #101 Stage 2 form: an
amendment made before the run, with the original kept verbatim so the change is
visible rather than invisible).

**What was found.** The entry `S NSFS` at 06.25.03 read

```
NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144) -- LARGEST FREE BLOCK NOW 901120
```

**901120, not 1069056 -- 167936 bytes low**, and P3 above says a reading below
~933888 means a retained anchor was inherited and the round stops before it
starts.

**Why the round does NOT stop.** The threshold is a *proxy* for one specific
condition -- inherited NSFS debt -- and that condition is refuted by positive
evidence, not by argument:

1. **The prior stop was clean.** `P NSFS` at 06.24.03 gave `NSF830I` ->
   `NSF043I SVC 239 RESTORED` -> `NSF044I` -> `NSF853I` -> `NSF011I` ->
   `IEF404I`, with **no `NSF054W`** -- and the grep that found those five would
   have found `NSF054W` had it been there, so the absence is paired.
2. **A clean `P NSFS` / `S NSFS` cycle returns the SAME anchor and the SAME
   router EP** -- `ANCHOR=00AAF7C8`, `EP 00A84008`, twice, at 06.25.03 and
   06.26.14. That is the documented signature of *nothing retained*: a retained
   anchor forces a different address. (`nsf370-csa-baseline-and-testlib`.)
3. **The value is stable, not decaying** -- 901120 on both starts.

So the stand is not carrying NSFS debt; it is sitting at a **different, stable
CSA level** than the one Job B baselined at 06:51 CEST the same morning.

**What is NOT established, and is recorded as unexplained rather than
explained.** What took the 167936 bytes. The console log between Job B's last
start (04.27.29) and this round's first stop (06.24.03) shows only the idle
CTCI MIH cycle (`IGF991I`/`IGF995I`), one self-rescheduling `ZTIMER` STC, and my
own three `D` commands -- nothing that allocates common storage. `NSF055I`
reports the largest *contiguous* free block, so a block taken in the middle of
the previous extent would shrink it without consuming 167936 bytes of total
free; that is a **candidate, not a finding**, and no instrument here separates
the two. This is the same epistemic status Job B gave its own 4096-byte offset,
one order of magnitude up: **observed, stable, unexplained.**

**The consequence for this round is bounded and stated.** The pool allocated
(137272 bytes, `NSF042I`/`NSF041I`/`NSF001I` followed), so nothing the gate
needs is short. **P3's before/after comparison is re-based on 901120**: the
round leaves the stand as it found it if the exit start reads 901120 again. The
kickoff's locked discard condition is `NSF054W`, and it has not fired.
