# Procedure — ADR-0040 row 2 (DEAD, available-ASID) live proof

**This is a PROCEDURE, not a test.** It is operator-driven, has no deterministic batch
form, and is **not** in `make test-mvs`. It belongs at **milestone gates**. Anywhere this
row's coverage is described, it must be called a procedure — a coverage kind that is not
named is read as a test a year later.

**What it proves.** A **real** client address space dying with a request genuinely
outstanding is classified **DEAD** by the ADR-0040 guard, is **never POSTed into**, and has
its request slot and in-flight count reclaimed. It ran **6 of 6** in the M5-2c2 mapping
round (`docs/measurements/m5-2c2/`).

**Why it is not a test.** Row 2 needs an address space that really ends. A batch client runs
in an **initiator**, which does not end when the job does — its recorded ASCB stays LIVE
forever (M5-2c1 stage a), so row 2 cannot be produced from a batch gate at all. An STC can
be made to die, but starting it, cancelling it and then delivering the datagram that
completes its parked request are three operator actions with no batch equivalent.

**Related coverage.** Row 1 is a live named probe (`TSTDEATH` on NSFV). Rows 3 and 4 are
host-pinned only and are **not live-producible** — see `test/mvs/tstdeath.c`'s header for the
full map. The classifier's arithmetic for every row is pinned in `test/tstreqx.c`.

---

## The rig (already in the repo)

| piece | where |
|---|---|
| client program | `test/mvs/tstappd.c` → module `TSTAPPD` in TESTLIB |
| STC procedure | `jcl/TSTAPPDS.jcl` → copy to `SYS2.PROCLIB(TSTAPPDS)` |
| optional driver | `docs/measurements/m5-2c2/park.sh` (automates the steps below) |
| optional ASVT reader | `docs/measurements/m5-2c2/rowwatch.py` (ground truth, read-only) |

## Prerequisites

1. `NSFS` started, and its CTCI device up — `NSF210I CTCI 0500/0501 UP` in the console log.
   The completing datagram arrives over that link; without it the procedure cannot finish.
2. **`TSTAPPD` present in TESTLIB — check this POSITIVELY, before starting anything:**

   ```
   zowe zos-files list all-members "IBMUSER.NSF370.V0R1M0D.TESTLIB"
   ```

   `TSTAPPD` must be in the list. If it is not, deploy it with
   `make test-mvs ARGS="--only TSTAPPD"` (which also runs it once, harmlessly — the default
   arm leaves one app slot registered).

   **`--only` REPLACES the whole test library**, so `TSTAPPD` and the Stage-0 set cannot be
   resident at the same time. **Operational consequence, for whoever plans a milestone
   gate: this procedure and the NSFV Stage-0 round CANNOT RUN BACK TO BACK — a redeploy
   sits between them, and it is a step to schedule rather than a surprise to rediscover.**
   A gate that wants both is: deploy the Stage-0 set → run the NSFV round → deploy
   `TSTAPPD` → run this procedure (or the reverse), never one continuous sequence.

   *This check was added after executing the procedure from its own text: the prerequisite
   was originally written to be discovered by `S TSTAPPDS` drawing `IEA703I 806-4 ... MODULE
   ACCESSED TSTAPPD`, which is discovery-by-failure — it costs a start, an abend and a
   restart, and an operator who did not know the remedy would read it as a broken rig.*
3. `SYS2.PROCLIB(TSTAPPDS)` installed from `jcl/TSTAPPDS.jcl`.
4. A shell on the CTCI host peer (`mvsdev`) that can reach `192.168.200.1`.

---

## Steps, with the expected output at each

### 1. Park a real request

```
S TSTAPPDS,P=PARK
```

Expect, within ~15 s:

```
+TSTAPPD: PARK ARM -- ASCB=xxxxxxxx ASID=xxxx SOCKET=0
+TSTAPPD: PARK ARM -- BLOCKING RECVFROM ON PORT 7799, ISSUE C <jobname> NOW
```

**Record the ASCB and ASID from that line.** Every later reading is checked against them; a
report about a different address space is not a measurement.

### 2. Prove the request is genuinely outstanding — BEFORE the cancel

```
F NSFS,STATS
```

Expect the **conjunction**:

```
NSF813I BUSY=1 BUSYSLOT=<n> INFLIGHT=1 ... REAPED=<r>
```

**All three parts matter, and `PENDING` alone would not do.** A slot can read `PENDING`
while it is already in service, so `BUSY=1` **and** a named `BUSYSLOT` **and** `INFLIGHT=1`
together are what establish that a request is published, dispatched and parked. **Record
`REAPED=<r>`** — the reap is proven by this counter moving, not by the message alone.

If `INFLIGHT=0`, the client is not parked: step 1 did not take, and continuing would prove
nothing. Stop and restart from step 1.

### 3. Kill the client

```
C TSTAPPDS
```

Expect:

```
IEF450I TSTAPPDS TSTAPPDS - ABEND S222 U0000 - TIME=hh.mm.ss
```

The address space is now gone. Its request is still published, its slot still held, and its
in-flight count still taken — exactly what a client that died mid-request leaves behind.

*(Optional ground-truth check, read-only: `python3 rowwatch.py <asid-dec> <ascb-hex> 5 1.0`
from `docs/measurements/m5-2c2/` should report `DEAD-row2-avail` — the ASVT entry's AVAIL
bit set. This reads the same field the guard reads, from outside, and is the independent
witness that the row really is row 2 and not row 3.)*

### 4. Deliver the completing datagram

The guard looks when the request **completes**, and nothing else makes it look. From the
host peer:

```
printf 'X' | nc -u -w2 192.168.200.1 7799
```

### 5. The result

Expect on the console, within ~1 second of the datagram:

```
NSF050I CLIENT DEAD (ASCB=xxxxxxxx ASID=xxxx) -- REQUEST REAPED
```

The ASCB and ASID **must match** the ones recorded in step 1.

Then:

```
F NSFS,STATS
   NSF813I BUSY=0 BUSYSLOT=-1 INFLIGHT=0 ... REAPED=<r+1>
```

`INFLIGHT` back to 0, the slot released, `REAPED` incremented by exactly one. **The counter
is the proof; the message is the narration.**

---

## What a broken guard looks like

The point of listing these is that a run can otherwise pass **for the wrong reason**.

| symptom | what it means |
|---|---|
| **No `NSF050I`, and `SERVED` incremented instead** | the guard answered **LIVE** for a dead client. The reply was POSTed into a CSA word nobody waits on; the slot is stuck `DONE` and `INFLIGHT` stays 1, **permanently** (the 40-CHK leak). This is the failure this row exists to catch. |
| **`NSF051W ... REQUEST HELD`** | the guard answered **UNKNOWN**. Not a reap: `INFLIGHT` stays 1 and the slot is held. Safe, but wrong for this input — a real dead client should be decidable. |
| **`NSF050I` present but `INFLIGHT` still 1** | the reap fired but did not complete its checklist. |
| **Nothing at all after step 4** | the datagram did not arrive. Check the CTCI link and the port; this is an environment failure, **not** a passing run. |

**The one that cannot happen by accident:** `INFLIGHT` and the slot returning to zero/FREE
**without** an `NSF050I`. Only the reap path returns them, so counts-returned is not
something a broken guard can produce while looking healthy.

---

## Limits, stated

- **Operator-driven.** Three manual actions, one of them on the host side. There is no
  deterministic batch form, which is why this is not in the matrix.
- **The window is unbounded and event-bounded.** Between step 3 and step 4 the identity is
  stale and unexamined, and the guard does not look until the request completes. If another
  address space starts in that window it may take the dead client's ASID **with the same
  ASCB**, and the guard will then correctly-but-uselessly read **LIVE** (M5-2c2 stage a:
  every one of 9 reuses restored the identical pair). **On a busy system, do steps 3 and 4
  close together**, and if step 5 shows no `NSF050I`, check whether an address space started
  in between before concluding the guard is broken.
- **This proves row 2 only.** It says nothing about rows 3 and 4, which are not
  live-producible, or about row 1, which `TSTDEATH` proves.
