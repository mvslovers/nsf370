# Job A §1.3 — the wire arm, run with the wire up

**Date:** 2026-09-15 · **Stand:** MVSCE-DEV on mvsdev, real CTCI pair 0500/0501,
MTU 1500, NSFS STC00187
**Predictions:** `predictions.md`, written before any deploy and **not edited**
afterwards; one amendment appended before any job was submitted, with the
original kept verbatim.
**Proof kind: LIVE.** **No source changed** — `test/mvs/tstrqx2.c` is
byte-identical to the merged Job A gate (`e2c2b65`), and `project.toml`,
`jcl/RQX2A.jcl`, `jcl/RQX2B.jcl` are untouched. The round is the same gate on a
stand whose interface is up.

**Result: the wire arm ran and is clean.** `wire=7 ok=7` and `dirty=0` on both
clients, **A 19/19 and B 24/24, both CC 0000**, with 14 datagrams on the host's
own `tcpdump` and the STC's counters reconciling exactly.

---

## Against the predictions

| | prediction | result |
|---|---|---|
| **P0** | the deploy is seen to take effect, fingerprint stated | **held** — CC 20 stage signature, §1 below |
| **P1** | `n_wire > 0`, `n_wire_ok == n_wire`, `dirty = 0` | **held** — `wire=7 ok=7 dirty=0` both clients |
| **P1** | assertion totals move 18→19 (A), 23→24 (B) | **held** — exactly |
| **P1** | ~6 in-range `SENDTO`s per client | **7** — the prediction was one low; §3 |
| **P2** | everything Job A proved reproduces | **held** — §4 |
| **P3** | stand left as found | **held**, against a **re-based** entry value; §5 |

---

## 1 — P0: the deploy, and why neither documented tell applies

Said in `predictions.md` before the run and repeated here because it is the part
a later reader needs: **this round has ONE build.** `main`'s `TSTRQX2` *is* Job
A's, byte for byte. So §5's documented tell — *identical values across
supposedly different builds* — has nothing to compare, and its inverse — *a
value neither pure build can produce* — has no second error set to discriminate
against. Neither is evidence here, and neither was quoted as if it were.

**The fingerprint is presence and freshness of the TESTLIB member**, and it is
two positive observations:

```
  TEST       BATCH          TSO
  TSTRQX2    FAIL CC 20     FAIL CC 20
  job MBTTEST JOB00377  | assertions (batch+tso): 2 PASS, 2 FAIL
```

```
TSTRQX2: NO ROLE ('') -- nothing ran
TSTRQX2: GATE SKIPPED -- CC 20, NOT a pass
```

That is Job A's own no-PARM signature. **The matrix reading FAIL rc=20 is
expected, not a red round** — the gate returns 20 when it could not run, so a
missing PARM can never report a pass. The alternative to a staged member is
`IEA703I 806-4`, which is loud; it did not appear.

**`n_wire > 0` is NOT used as the deploy check.** It is the measurement. That
the interface was up was established *independently and beforehand*:
`D U,,,500,4` → `500 CTC A-BSY / 501 CTC A`, and `NSF210I CTCI 0500/0501 UP …
MTU 1500` / `NSF211I INTERFACE LNK1 CUU 0500 UP` at every `S NSFS`.

## 2 — the result

```
A (JOB00378):  bulk=229125 txerr=0 wrong=0 dirty=0 | wire=7 ok=7
               === TSTRQX2: 19/19 passed ===        CC 0000
B (JOB00379):  bulk=229312 txerr=0 wrong=0 dirty=0 | wire=7 ok=7
               === TSTRQX2: 24/24 passed ===        CC 0000
```

The two assertions that exist only on this branch:

```
PASS: 1.3: the in-range (real datagram) case ran
PASS: 1.3: and every in-range SENDTO reported its full length -- the one case
           where an op actually READS the landing area
```

**The totals moving 18→19 and 23→24 is the arithmetic that separates this run
from Job A's**, and it was written down before the run rather than noticed
after: the wire branch carries two CHECKs where the no-interface branch carries
one, so a run that skipped the arm **cannot** present as one that took it. The
skip is asserted in positive form as well, and neither client printed it.

## 3 — the independent witness, and it is not the stack's own word

`n_wire_ok` is a **stack-level** count: the STC saying its `SENDTO` returned
1024. Paired with two independent readings:

**The host's own capture** (`tcpdump -ni tun0 'udp port 9999 or icmp'`, run on
mvsdev, `run1-tcpdump-tun0.txt`):

```
7 datagrams  192.168.200.1.7803 > 192.168.200.2.9999: UDP, length 1024   (A)
7 datagrams  192.168.200.1.7804 > 192.168.200.2.9999: UDP, length 1024   (B)
14 × ICMP 192.168.200.2 udp port 9999 unreachable, length 556
```

14 = 7 + 7, matching `wire=7 ok=7` on each side; every length is `XA_WIRE_N`
1024; and the timestamps are 15.000 s apart, which is the checkpoint grid. The
14 port-unreachables are a **second-order confirmation the datagrams actually
arrived** — nothing listens on 9999, which is why that port was chosen.

**The instrument was validated before the run, not trusted:** two `ping`s from
mvsdev to the guest appear in the same capture with the guest's replies, so a
silent capture would have been a real null rather than a broken filter.

**The STC's counters** (`F NSFS,STATS`, before and after, both on STC00187):

| counter | before | after | reconciles as |
|---|---|---|---|
| `NSFUDP out` | 0 | **14** | 7 + 7, the wire arms |
| `LNK1 out` / `NSFIP out` | 0 | **16** | 14 UDP + 2 ICMP echo replies (the control pings) |
| `LNK1 in` / `NSFIP in` | 0 | **16** | 14 port-unreachables + 2 echo requests |
| `NSFIP noroute` | 0 | **0** | no `EHOSTUNREACH` — the Job A condition is gone |
| `NSFSOC opens/closes` | 0/0 | **5/5** | leak gate clean |
| `NSFREQ recv` | 0 | 458471 | 229125 + 229312 + 14 + the lifecycle verbs |

**Every one of those totals is exact**, including the two pings I introduced —
which is a sharper check than a round number would have been.

**One prediction was arithmetically one low, and it is corrected rather than
rounded away.** `predictions.md` says *"6 checkpoints and ~6 in-range
`SENDTO`s"*; the run gives **7**. `next_chk` starts at **0**, so the grid is
t≈0 plus six 15 s points. Job A's own table has the same seven rows — the
prediction mis-derived a number that was already on the record it was written
from.

## 4 — P2: everything Job A proved reproduced

§1.1 — both clients progressed in **every** interval (A 40437/39255/38342/
37699/35904/37487; B 40515/39459/38303/37612/35967/37455), and the sampling's
own positive control fired. §1.2 — B refused `EBADF` on A's `00010000` while
**A was served `rc=0` on that same value** (the control that makes the refusal a
refusal), B served on its own `00010001`, A refused `EBADF` on its own after
TERMAPI, and B's datagram socket **driven**, its stream socket answering
`EINVAL` not `EBADF`, its app slot granting a new socket. §1.3's copy pair —
`txerr=0 wrong=0 dirty=0` over 458 437 bulk calls.

`COLLISIONS=228226` with `EXHAUSTED=0` — the two clients really were
interleaving on a serialised service, not queuing behind an empty pool.

**These are Job A's results reproducing, not re-proved.** A red row here would
have been a regression to investigate; none appeared. The bulk counts differ
from Job A's (229 125 vs 214 648) and that means nothing — Job A reports no
throughput figure, and this round reports none either.

**A second aside corrected rather than left standing:** P2's falsification
clause anticipated *fewer* bulk calls, since six real datagrams now ride the
window. The run gave **more** — 229 125 against 214 648. Immaterial (no
assertion depends on it, and neither round reports a rate), but the clause
guessed a direction it had no basis for, and the honest note is that a
throughput difference across two rounds months and one stand-state apart is
unattributable in either direction.

## 5 — P3: the stand, and an entry value that fired the stop clause

**The entry reading was `901120`, not the `1069056` Job B baselined the same
morning — 167936 bytes low — and `predictions.md` says a reading below ~933888
means a retained anchor and the round stops.** The amendment there records why
it did not, and the reasoning is not repeated here beyond its evidence:

- the prior `P NSFS` was clean (`NSF043I`, no `NSF054W`, the absence paired with
  five present messages the same grep found);
- a clean `P NSFS` / `S NSFS` cycle returned the **same anchor `00AAF7C8` and
  the same router EP `00A84008`** — the documented signature that nothing is
  retained;
- the value was **stable**, 901120 on both starts.

So the threshold's *proxy* fired while its *reason* was refuted. **What took the
167936 bytes is not established**, and is recorded as unexplained: the console
log between Job B's last start and this round shows only the idle CTCI MIH
cycle, a self-rescheduling `ZTIMER`, and my own three `D` commands.
`NSF055I` reports the largest *contiguous* block, so fragmentation is a
**candidate, not a finding**, and no instrument here separates it from
consumption. Same epistemic status as Job B's own 4096-byte offset, an order of
magnitude up.

**Exit:** `P NSFS` clean (`NSF043I`, `NSF011I`, `IEF404I`, **no `NSF054W`**),
then `S NSFS` → **`901120` again, same anchor, same EP**. Entry equals exit: the
round left the stand as it found it.

**Zero dumps.** `IEA995I` 0, `IEF450I` 0, `NSF054W` 0, `NSF900E`/`NSF903I` 0
across the whole round window — **with the same grep finding 7 `IEF404I`, 3
`NSF043I` and 3 `NSF055I` in that window**, so the zeros are nulls and not a
pattern that matches nothing.

Host: **3491 PASS / 0 FAIL**, unchanged — a **no-regression check only**, since
`tstrqx2.c` is `host = false` and nothing else moved.

## 6 — the environment, as a condition

| | |
|---|---|
| emulator | **ONE** — `hercules -f conf/local.cnf` out of `~/MVSCE-DEV`, PID 1098. No Hercules instance was started, stopped or restarted. |
| task set | JES2, NET, TSO, HTTPD, UFSD, FTPD — the **restored** set, identical before and after |
| `tun0` | **up**, `192.168.200.2 peer 192.168.200.1` |
| CTCI | 500 `A-BSY` / 501 `A`; 502/503 offline |
| entry pool | **901120** — see §5; **not** the 1069056 / 1073152 pair, and the offset is unexplained, not explained |
| TESTLIB | `--only TSTRQX2`, so it holds that member **alone** — the documented consequence of `--only`, which replaces the library |

## 7 — what this round discharges, and what it does not

**Discharges — `awaiting-ctci-pair.md` item 2, Job A §1.3's protocol-operation
gap.** Job A proved the transport's **copy pair** in full but could not reach an
op that *reads* `g_land`: its bulk verb is a non-blocking `RECVFROM` answered
`EWOULDBLOCK` before `udp_recv` touches `ubuf`. The in-range `SENDTO` is the one
case where a protocol op reads the landing area, and it now ran — 14 times,
verified on the wire, with both clients' patterns intact after every call.

**Does NOT discharge, and must not be read as having:**

- **d1 §2.3** — the SELECT arms' stimulus, `awaiting-ctci-pair.md` item 1.
  Untouched here. **#107 assigns it to a d1 round, after the flip**, and (e)
  does not cover it.
- **Throughput or latency of anything.** No figure here is a baseline and none
  is comparable with Job B's.
- **Concurrent service.** The model is unchanged and serial.
- **The partial-write residue shape** — an op writing FEWER bytes into `g_land`
  than `xlen`. The in-range `SENDTO` reads the full staged surface; the detector
  is the identity round trip, not the residue.
- **What consumed the 167936 bytes of CSA** (§5).
- **A milestone flip.** Nothing here flips to proven; the M5-2 flip is Mike's
  and follows this round.

## 8 — round hygiene: two instrument faults of mine

**A status-poll query that returned nothing for 18 iterations.** `zowe
zos-jobs view job-status-by-jobid … --rff status,retcode --rft table` printed
empty lines throughout, which reads exactly like *"still running"* — the
absent-vs-succeeded shape (§8.5) aimed at my own instrument. Caught by putting a
**known-completed job through the same query**: the control was empty too, so
the fault was the query and not the jobs. The `--rfj` form answered immediately
and both jobs had already been `OUTPUT CC 0000`.

**`zowe` is broken under the host's default Node.** Node v26 removed
`util.isNullOrUndefined` and every `zowe` call dies in `TextUtils.js`. Every
command in this round ran with `PATH="$HOME/.nvm/versions/node/v22.15.1/bin:$PATH"`
prefixed **in the same shell invocation**, since shell state does not persist
between them. The running instance is also `~/MVSCE-DEV/`, not the `~/MVSCE/`
every earlier record names, so `mvslog.txt` and `hercules.log` are there.

---

## Files

| | |
|---|---|
| `predictions.md` | written before any deploy; amendment appended before any submit |
| `stage-JOB00377.txt` | the no-PARM stage run — P0's fingerprint |
| `run1-A-JOB00378.txt` | role A, CC 0000, 19/19 |
| `run1-B-JOB00379.txt` | role B, CC 0000, 24/24 |
| `run1-tcpdump-tun0.txt` | the host's capture, incl. the control pings |
| `run1-stats-after.txt` | `F NSFS,STATS` after the run, 52 counters |
