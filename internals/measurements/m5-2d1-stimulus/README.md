# d1 §2.3 arms 1 and 2, re-run with the stimulus in evidence

**Date:** 2026-09-15 · **Stand:** MVSCE-DEV on mvsdev, CTCI 0500/0501, MTU 1500,
NSFS STC00191
**Predictions:** `predictions.md`, written **before the test edit existed** and
before any deploy. **Not edited afterwards.**
**Proof kind: LIVE.** A **d1 round**, not (e) — #107 assigned it there. M5-2 is
already flipped; this discharges one of the two properties it names as unproven.

**Result: both arms green and non-vacuous. A CC 0000 (9/9), B CC 0000 (13/13).**
The 2026-09-03 annotation is discharged: A's own SELECT reports **the same
socket READY** across both of B's arms, so B's "not ready" is a refusal and not
an idle listener.

| | prediction | result |
|---|---|---|
| **P0** | deploy seen to take effect, fingerprint stated | **held** — §1 |
| **P1** | arm 1 poll: foreign not ready, own served, no error | **held** — `foreign.ready=0 own.ready=2 errno=0` |
| **P2** | arm 2 parked: does not complete, with a real EDGE inside the park | **held** — `rc=0 ready=0`, two graduations inside the window |
| **P3** | A's own result read; skip rather than fail if no stimulus | **held** — A 9/9, ever-ready=1, ready-at-last-poll=1 |
| **P4** | stand left as found | **held** — entry = exit = 1069056, no `NSF054W`, zero dumps |

---

## 1 — P0: the deploy, and the fingerprint

**`NSF.LINKLIB` was NOT redeployed, and did not need to be.** `src/`, `asm/`
and `include/` are untouched; `make test-mvs` targets **TESTLIB**, which no STC
holds. The `P NSFS` / `S NSFS` cycle was done for a different reason — §5's
generation-window condition, which arm 2.2's `RANGE INADEQUATE` guard depends
on, and which this round's `SWEEP post-own ... 00010001` confirms was met.

**Both §5 tells are inapplicable here and are not quoted as if they carried:**
there is one build of the changed module, so *identical values across
supposedly different builds* has nothing to compare, and *a value neither pure
build can produce* needs two pure builds to enumerate across.

**The fingerprint is a value the old TESTLIB cannot produce:** `role_a`'s
**`TSTD1B: A POLL n RC=… READY=…`** console lines. No previous build of this
module has ever emitted one, and 90 of them are in the log. The no-PARM stage
also gave its `NO ROLE -- SKIPPED` / **CC 20** signature (JOB00383, batch+TSO,
2 PASS 0 FAIL).

**One sequencing fault of mine, reported not smoothed:** the **first** stage run
(JOB00381) was made with NSFS stopped, so `nsfreqc_init` issued a restored — i.e.
unassigned — SVC 239 and the job took **`ABEND SFEF`**. That is documented
behaviour (§5: Stage-0 clients abend `SFEF` with no router), not a defect and not
a product finding; it is also the single `IEF450I` in the round window. The stage
was re-taken after `S NSFS` to get the predicted CC 20 control.

## 2 — A is B's positive control, and the transition is the stimulus arriving

`role_a` now polls **its own** descriptor with a 0/0 `nsf_select` (never parks,
so it cannot wedge the serialised service) and WTOs every result.

```
 9.08.17  A POLL 0 RC=0 READY=0      <- the instrument's NEGATIVE control:
 9.08.19  A POLL 1 RC=0 READY=0         the poll can say NO, so a later YES
 ...                                    is a reading, not a constant
 9.08.31  A POLL 7 RC=0 READY=0
 9.08.33  A POLL 8 RC=1 READY=1      <- the stimulus ARRIVES
 ...
 9.11.37  A POLL 89 RC=1 READY=1     <- still ready at the final poll
```

**Poll 8 flips in the same second the driver logged connect #1**
(`16:08:33.197` host clock = `9.08.33` MVS local; the two clocks differ by a
fixed offset and agree to the second here). A was then continuously ready from
`9.08.33` to `9.11.37`, which **brackets both of B's arms** (`9.08.41`–
`9.09.03`).

**The bracket is the evidential form, and §0.2 of `predictions.md` says why it
had to be** — the kickoff's "an A-ready line *inside* each arm's window" is
unsatisfiable: arm 1's window is a single call, and arm 2's window is exactly
when A cannot answer, because B's parked SELECT holds `g_busy` (serialised
service, ADR-0042 §10; measured in #101 Stage 2 as V unserved with `SERVED`
frozen). The bracket implies the interior because the stimulus is **monotone**:
A never accepts, B *cannot* accept A's socket — the property under test is
itself that guarantee — and A closes only afterwards.

**The monotonicity condition was checked, not assumed:** the capture shows
**zero RSTs** (an RST would dequeue the child via `tcp_do_reset`) against a
positive control of 15 flag-bearing lines. And the FINs at `16:09:33` followed
by A still `READY=1` at `9.11.37` is **live confirmation of the "a FIN is
tolerable" claim** that `predictions.md` made from source — the child stays on
the `acceptq` in CLOSE_WAIT and the listener stays read-ready.

## 3 — the arms

```
SWEEP pre-own:  0 reached out of 128 attempts
SWEEP post-own: 1 reached out of 128 attempts  00010001
B's own = 00010001, so A's is derived as 00010000
foreign(00010000) rc=-1 errno=9 | unknown rc=-1 errno=9
SELECT poll:   rc=1 errno=0 foreign.ready=0 own.ready=2
SELECT parked: rc=0 errno=0 ready=0
=== TSTD1B: 13/13 passed ===
```

**Arm 1 (poll).** The foreign entry is **not ready while A is ready** — that is
the whole content of the round. B's own entry **is** served (`ready=2`), which
is the in-arm control, and the call is not an error, so SELECT does not become
an existence oracle.

**Arm 2 (parked) — and the EDGE is what makes it the re-scan path.** B parked
for 20 s; **two further connections completed inside that window**, at
`16:08:41.203` and `16:08:44.204` against a window of `16:08:41.202` →
`16:09:03.214` (host clock throughout — no cross-clock arithmetic in the
load-bearing step). Each graduation fires `soc_notify_ready(ls, SEL_READ)`
unconditionally (`src/nsftcp.c:1153`), which makes `nsfsel_on_notify` re-scan
**every** parked SELECT, B's included, with B's identity from the SELCB. B's
SELECT did not complete and the entry stayed not-ready.

`NSFTCP passiveopen 3` / `established 3` confirms three children graduated —
i.e. **three real pokes**, one before the arms and two inside the park.

**No "a poke CAN complete a parked SELECT for its owner" control was built.**
That positive control already exists on this stand and this code path: #101
Stage 2's role W completed `RC=1 MASK=00000001` on a host connect. Rebuilding it
would bolt a wire-dependent arm onto `role_b` for no evidentiary gain.

## 4 — THE FINDING THAT NEARLY MADE THE ROUND VACUOUS

**A passive child takes a socket-table slot**, and it would have broken the
round silently. `tcp_child_create` (`src/nsftcp.c:1397`) calls **`soc_create`**
for every incoming connection and stamps `cs->apptok = ls->apptok`.

`role_b` derives A's descriptor as **`own - 1`** (#106's repair of defect 3).
That holds only while A and B are adjacent in the table. **A connect made
before B allocated its socket would insert a child between them** — A 0, child
1, B 2 — and `own - 1` would then name **the child**.

**It would have gone green.** The child is foreign to B too, so every refusal
assertion would still have passed — while the round tested the wrong socket,
and against a **non-listener**, for which `tcp_poll` reports not-ready *to its
owner as well*. Arm 1 would have been vacuous in a brand-new way.

So the stimulus is fired **after** B's sweep, on a marker B emits itself
(`B SWEEP DONE A-DESC …`). Read from source while designing and written into
`predictions.md` §0.1 before any code existed. **Confirmed live:** B derived
`00010000`, and `NSFSOC opens 5` = A's listener + B's socket + **3 children**.

`role_b`'s derivation and **every one of its assertions are untouched** — only
the window moved, which is what the kickoff asked for.

## 5 — the stand

| | |
|---|---|
| emulator | **ONE** — PID 1098, `hercules -f conf/local.cnf`, cwd `~/MVSCE-DEV`. None started, stopped or signalled. |
| host | rebooted 2026-09-04; `tun0` up, `192.168.200.2 peer 192.168.200.1` |
| task set | JES2, NET, TSO, HTTPD, UFSD, FTPD — the restored set |
| CTCI | 0500/0501 up at MTU 1500 (`NSF210I`/`NSF211I`) |
| CSA entry | **1069056**, exit **1069056**, same anchor `00A8B7C8`, same router EP `00A8B248` |

**This is a GATE, not a measurement**, so the CSA marker does **anchor
detection only** — free storage is not an input to any assertion here (#119's
correction; for a measurement the comparability argument would hold and the
round would stop). **No `NSF054W`.**

**A data point for #120, and it runs against the drift's assumed direction:**
the same stand read **901120** this morning (the Job A wire round) and reads
**1069056** now — **the 167936 bytes came back**, and the anchor and router EP
returned to the values they had before. So the mover is **not monotone**, and
"something took storage" is not the only shape the series has.

**Zero dumps.** `IEA995I` 0, `NSF054W` 0, `NSF900E` 0; one `IEF450I` — JOB00381,
the SFEF explained in §1, before the arms — against a positive control of 19
`IEF404I`, 5 `NSF043I` and 6 `NSF055I` found by the same grep.

Host **3491 PASS / 0 FAIL**, unchanged — a **no-regression check only**, since
`tstd1b.c` is `host = false`.

## 6 — round hygiene: one instrument trap, met twice

**`pgrep -f <pattern>` matches the ssh shell's own command line.** It first
reported *two* emulators (there is one — PID 1098, established with `ps` and
its args), and later reported the stimulus driver still running when its log
already ended with `END (sockets closed cleanly -- FIN, never RST)`. Worse, an
earlier `pkill -f 'tcpdump.*tun0'` **killed the shell that was about to start
tcpdump**, which is why the first attempt produced no capture and no error file.
Use `ps -eo pid,args | grep` and check against a positive control.

The capture instrument was validated with two control pings **before** it was
relied on, so a silent capture would have been a real null.

## 7 — what this discharges, and what it does not

**Discharges** the 2026-09-03 annotation on `docs/measurements/m5-2-d1-select/`
and **item 1** of `awaiting-ctci-pair.md`: arms 1 and 2 now have a stimulus in
evidence, on both the guest's side (A's own SELECT) and the wire's.

**Does NOT establish:**

- **d2**, the other named-unproven property of M5-2. Untouched.
- **That a poke completes a parked SELECT for its OWNER** — cited from #101
  Stage 2, not re-run here.
- **Concurrent service.** The model is unchanged and serial; §2's bracket
  argument depends on it being serial.
- **Anything about (e)** or any throughput figure. None is reported.
- **What moved the CSA reading**, in either direction (#120).
- **A milestone flip.** Nothing flips to proven.

## Files

| | |
|---|---|
| `predictions.md` | written before the test edit existed; §0.1 and §0.2 are the two source findings |
| `d1stim.py` | the stimulus driver — runs on mvsdev, watches the console marker, holds the connections open |
| `stage-JOB00383.txt` | the no-PARM stage, CC 20 — P0's control |
| `run1-A-JOB00384.txt` | role A, CC 0000, 9/9 — the readiness bracket |
| `run1-B-JOB00385.txt` | role B, CC 0000, 13/13 — the arms |
| `run1-d1stim.log` | the driver's host-clock log: triggers, connects, park window |
| `run1-tcpdump-tun0.txt` | the wire: 3 × 3WHS, zero RSTs, the control pings |
| `run1-stats-after.txt` | `F NSFS,STATS`, 52 counters |
