# Issue #64, step 64-0c — where the executive task actually is

**Measurement record, not an ADR and not a decision.** 64-0c fixes nothing. Decision 2 is
back with Mike and is not the evaluator's to apply.

Round: MVSCE on `mvsdev`, 2026-08-26, module built from `m5-64-0c-suspension-point`
(64-0b's instrumentation with `BUSY` / `BUSYSLOT` moved to `NSF813I`).
Console times are the MVS clock (UTC-5); host times are CEST.

**Not read:** `nsf-64-diagnosis-memo.md` is still not in the repo and was not supplied to
this session. It has now been absent for three rounds (64-0, 64-0b, 64-0c); every
reference to it below is to the two measurement records that *are* in the tree.

---

## 1. The instrument repair

`BUSY` / `BUSYSLOT` moved from the end of `NSF812I` — the longest WTO in the system — to
the **front** of the short `NSF813I`. The Hercules console log truncates a message at
roughly 107 characters of text, so `BUSYSLOT`'s value was cut exactly when the counters
grew to seven digits, i.e. exactly during the investigation it was added for.

The repair is a **position**, not only a shorter line: truncation eats the tail, so the two
fields the round turns on now lead the line, `INFLIGHT` follows (load-bearing in every
stall reading so far), and what can still be lost is `TMRQ` / `EXHAUSTED` / `COLLISIONS` /
`REAPED`, where losing one costs nothing.

The `\n` alternative works — libc370's `vwtof` splits its text on newlines and issues one
`wto()` per line (`src/clib/vwtof.c`, verified in the source) — and was rejected because
the second line would carry no message id, which is unreadable to a grep and odd on a
console, where `NSF813I` already exists and is where this context belongs.

Measured after deploy, and this **is** the deploy-took-effect check — by the *field*, not
the message id, because 64-0b's module already had both lines:

```
NSF812I WAKEECB=009DCD10 POSTED=N EVTPASSES=214 WAKEPOSTS=0 WPREG=Y SERVED=0
NSF813I BUSY=0 BUSYSLOT=-1 INFLIGHT=0 TMRQ=0 EXHAUSTED=0 COLLISIONS=0 REAPED=0
```

`NSF812I` is now 89 characters at seven-digit counters; `NSF813I` stays under 100 even
with ten-digit `EXHAUSTED` and `COLLISIONS`.

---

## 2. `/.dm` CANNOT read the STC's task state — and the earlier reading was HTTPD's

This is the first substantive finding of the round and it invalidates the obvious approach.

The httpd display modules run **in HTTPD's address space**. `ASXB`, `TCB` and `RB` all live
in **LSQA, which is private** — and on this stand LSQA is mapped at the **same virtual
address in every address space**. Measured, not inferred:

| read | value |
|---|---|
| NSFS `ASCB` (from `anchor+X'14'` `server_ascb`) | `00FDD018`, `ASCBASID` = `000B` |
| HTTPD `ASCB` (from `PSAAOLD`, PSA+X'224') | `00FF93E8`, `ASCBASID` = `000A` |
| `ASCBASXB` (+X'6C') in **both** | **`009DF300`** |
| `PSATOLD` (PSA+X'21C') — HTTPD's own current TCB | `009B2DE0` |

A first pass chased `009DF300` through `/.dm` and got a perfectly coherent `ASXB`
eyecatcher, a self-consistent TCB chain and `ASXBTCBS=15` matching the chain length. It was
**HTTPD's own** — its `PSATOLD` `009B2DE0` is the *last* TCB of that chain. NSFS's real
ASXB says `TCBS=6, LTCB=009CE0C0`.

**The trap is that the wrong answer looks completely healthy.** Eyecatcher present, chain
self-consistent, count matching. Nothing about the reading says "different address space".
This is the project's most expensive failure class in a new place, and the only thing that
caught it was reading `PSAAOLD` to ask *whose* address space the display module was in.

### The path that does work — and it needs no CPU stop and alters nothing

```
ASCB              -> /.dm            SQA is common, so this is genuine
ASCBSTOR (+X'2C') -> the CR1-format segment-table origin, REAL
segment/page table-> Hercules `r`    real storage
ASXB / TCB / RB   -> Hercules `r`    at the DAT-translated real addresses
```

`ASCBSTOR` is documented in `IHAASCB` as *"TABLE LENGTH AND REAL ADDRESS OF SEGMENT TABLE
IN THE SAME FORMAT AS CONTROL REGISTER ONE"* — read live, not recalled. NSFS's is
`0FB68C00`: `STL` = `X'0F'` → 256 four-byte entries → 16 MB in 64 KB segments; `STO` =
`B68C00`.

DAT constants come from the **running Hercules build's** `esa390.h`
(`~/hercules/hyperion`), not from memory: `STD_370_STO 0x00FFFFC0`,
`SEGTAB_370_PTO 0x00FFFFF8`, `SEGTAB_370_INVL 0x00000001`, `PAGETAB_PFRA_4K 0xFFF0`,
`PAGETAB_INV_4K 0x0008`. `CR0` reads `C080EC40` → 4 KB pages, 64 KB segments, which the
observed 224-byte page-table stride (16 PTEs + the external page table) independently
confirms.

`r addr.len` requires only that the CPU be **online** (`hscemode.c` `abs_or_r_cmd`) — no
`stopall`, unlike `savecore`, which is rejected unless `CPUSTATE_STOPPED`. So the whole
reading is read-only with respect to MVS. LSQA is fixed, so the virtual→real page map is
built once and reused.

---

## 3. Every control-block offset, and where it came from

Computed by **IFOX00 from the `SYS1.AMODGEN` macros themselves** — jobs `CBOFF`
(`JOB02215`) and `CBOFF2` (`JOB02217`), both **CC 0000 with zero diagnostics** — as
`DC AL2(field-origin)` constants read out of the assembly listing. The b2 method.

| macro | field | offset |
|---|---|---|
| `IHAASCB` | `ASCBCPUS` / `ASCBASID` / `ASCBSTOR` / `ASCBASXB` / `ASCBDSP1` | `X'20'` / `X'24'` / `X'2C'` / `X'6C'` / `X'72'` |
| `IHAASXB` | `ASXBFTCB` / `ASXBLTCB` / `ASXBTCBS` | `X'04'` / `X'08'` / `X'0C'` |
| `IKJTCB` | `TCBRBP` / `TCBPKF` / `TCBFLGS` / `TCBFLGS4` / `TCBFLGS5` / `TCBTCB` / `TCBOTC` | `0` / `X'1C'` / `X'1D'` / `X'20'` / `X'21'` / `X'74'` / `X'84'` |
| `IHARB` (+`IKJRB`) | `RBBASIC-RBPRFX` | `X'40'` |
| | `RBFLAGS1` / `RBWCSA` / `RBINTCOD` | `RBBASIC-8` / `-4` / `-2` |
| | `RBSIZE` / `RBSTAB1` / `RBSTAB2` / `RBCDE` / `RBOPSW` / `RBWCF`=`RBLINK` | `RBBASIC+8` / `+X'0A'` / `+X'0B'` / `+X'0C'` / `+X'10'` / `+X'1C'` |
| `IHAPSA` | `PSAAOLD` / `PSATOLD` | `X'224'` / `X'21C'` |
| `IHACDE` (`SYS1.MACLIB`) | `CDNAME` / `CDENTPT` | `X'08'` / `X'10'` |

Bit constants, same source: `RBFTP` mask `X'E0'` — **PRB `X'00'`, SVRB `X'C0'`**, IRB
`X'40'`, SIRB `X'80'`, TIRB `X'60'`; `RBXWAIT` `X'40'` and `RBLONGWT` `X'04'` in
`RBFLAGS1`; `RBECBWT` `X'01'` and `RBTCBNXT` `X'80'` in `RBSTAB2`.

**`IHARB` is called alone and with its defaults.** With `SYS=AOS2` it builds `RBPRFX` and
then calls `IKJRB DSECT=NO,COM=YES`, which `ORG`s the OS/VS2 names back over `RBPRFX` — so
every RB symbol lands in one DSECT and every difference above is legal. Calling `IKJRB`
directly would have produced a second, disjoint `RBPREFIX`.

**Three of these cross-check exactly against M5-2b2's independently derived numbers** —
`RBBASIC-RBPRFX` = `X'40'`, `RBSIZE-RBBASIC` = 8, `RBEXSAVE-RBBASIC` = `X'60'` — and
`TCBRBP-TCB` = 0, `TCBPKF` = `X'1C'` (b1) and `ASCBASID` = `X'24'` (Stage-0c) match what
the tree already carried. Nothing here rests on a single derivation.

`TCBRBP` points at `RBBASIC`, not at `RBPRFX`: b2 measured `RBSIZE` off `TCBRBP` as 28
doublewords, a sensible RB, and this round reads `17dw` for every ordinary PRB the same
way.

### One thing that went wrong, and it is the project's own rule

The first `CBOFF` assembly failed with `IFO026 CHARACTERS APPEAR BETWEEN THE BEGIN AND
CONTINUE COLUMNS` — my banner comment lines ran past column 71, so IFOX00 read the *next*
card as a continuation and **swallowed it**. Statement-eating included the `IHAASCB` macro
call itself, which is why five offsets came back "undefined symbol" and one
(`AASXB`) assembled to `0000` with no complaint about the missing DSECT. CLAUDE.md §3's
column-71 rule, biting in a diagnostic job rather than in `asm/`.

---

## 4. Which TCB is the executive — identified, not assumed

NSFS's chain is **6 TCBs**, matching `ASXBTCBS`, ending at `ASXBLTCB`:

| # | TCB | `TCBPKF` | `TCBOTC` | RB `CDNAME` |
|---|---|---|---|---|
| 0 | `9DD148` | 00 | — | (common CDE — region control task) |
| 1 | `9DE3A0` | 00 | `9DD148` | (common CDE, SVC 116) |
| 2 | `9DE150` | 80 | `9DD148` | (common CDE) |
| 3 | **`9DC7B0`** | 80 | `9DE150` | **`NSFS`** |
| 4 | `9CE310` | 80 | **`9DC7B0`** | `CTHREAD` |
| 5 | `9CE0C0` | 80 | **`9DC7B0`** | `CTHREAD` |

**TCB3 is the executive**, by two independent facts: its RB's `CDE` names the program
`NSFS`, and TCB4/TCB5 — the two CTCI I/O subtasks — carry `TCBOTC = 9DC7B0`, i.e. they were
attached *by* it. That is the structure `nsfsmain.c` describes.

---

## 5. Two calibration references, taken on demand

The stall reading is worth nothing without states to compare it against, so both were
measured on the same instance minutes apart.

| | `POSTED` | pass rate | CP00 / CP01 PSW | TCB3's current RB |
|---|---|---|---|---|
| **REF1** fresh, before any request | **N** | ~10/s (heartbeat) | both **Enabled Wait**, `070E0000 00000000`, 0.36 MIPS | **PRB `9DCD10`, `WCF=1`, SVC 1, `RBFLAGS1=42` [XWAIT], `RBSTAB2=83` [ECBWT TCBNXT]** |
| **REF2** same STC, after requests | **Y** | ~8500/s (spinning) | both **Running Normal**, CP01 89 %, 63 MIPS | **SVRB `9DE7D0` on top of that PRB, `WCF=0`** |

REF1 is what *"waiting in `ecb_waitlist`"* looks like on this stand, and it reads exactly
as the seam predicts: a `WAIT` of count 1 over a longer ECBLIST sets `RBECBWT`
(*"wait for a number of events that is less than the total number waiting"*), and `RBXWAIT`
marks the explicit SVC WAIT. REF2 is a running task — the SVRB is transient, caught
mid-SVC, because a spinning loop is inside an SVC most of the time.

So the discriminator for the stall is sharp, and **stability across repeated samples** is
part of it: a stalled task that is genuinely suspended reads the same RB every time.

---

## 6. The stall, reproduced twice, read during the stall

### The arm that reproduces

**An idle stack does not stall.** 64-0's two arms, 64-0b's prescribed window and this
round's first 22 samples over ~8 minutes of post-workload idle all failed to reproduce —
that is now **four** non-reproductions in the idle configuration. What every stall on
record has in common is a **client parked with a published request**: #64's own
(`slot 0 PENDING, inflight=1`), 64-0b's stall 1 and stall 2, and both of this round's.

So the arm is the two-client gate re-run back to back. Both reproductions fired within
90 seconds of a gate round starting.

### The detector, and why it changed

64-0b detected a stall by an unanswered `F NSFS,STATS`, which is sound —
`nsfopr_drain` runs unconditionally on every pass, so an unanswered MODIFY *is* a stall.
But a MODIFY POSTs the cib ECB, and **`nsfsmain.c:316` does call `evt_set_operator`**, so
that ECB is in the executive's ECBLIST: sampling on a short cadence injects a POST into the
mechanism under test.

This round used a **passive** detector instead. `ASCBEJST` (ASCB+X'40', `CBOFF3`) is the
address space's accumulated job-step CPU time and lives in SQA, so `/.dm` reads it with no
POST and no cooperation from the STC. A spinning executive burns a core, so EJST climbs at
~1 CPU-second per second — measured at 4.2×10¹⁰ TOD units per 10 s = **1.02 s/s**, which is
also what calibrates the threshold. A stall reads **exactly zero**. The MODIFY is then
issued **once**, after the readings, to confirm.

### Stall 1 — 11 min 25 s of zero CPU; the MODIFY sat 9 min 14 s

| time (CEST / MVS) | event |
|---|---|
| 20:30:22 | EJST goes flat, `delta=0` |
| 20:30:42 | detector triggers (3 flat samples) |
| 20:30:46 / 20:31:20 / 20:31:53 | three full reading passes |
| 20:32:22 (13.32.23) | confirming `F NSFS,STATS` issued |
| 20:34:05 | `D A,L` taken during the stall |
| 20:35:12 | **fresh** page-table validity check |
| 20:39:34 | deep read — EJST still bit-identical |
| 20:41:36 | MODIFY **still unanswered** |
| 20:41:37 (13.41.37) | 3 host pings → **answered immediately**; first RTT **444 ms**, the rest 1.18 ms |

EJST last moved at 20:30:12 and was bit-identical until the ping — **11 m 25 s of exactly
zero CPU**. The MODIFY latency, 9 m 14 s, is the lower bound the detector can quote; both
are lower bounds, because the stall ended only when it was broken.

The MODIFY quoted from the console log, unanswered across every reading above:

```
0000 13.32.23 STC 1428  F NSFS,STATS
FFFF 13.41.37 STC 1455  NSF812I WAKEECB=40000000 POSTED=Y EVTPASSES=8624660 ... SERVED=375
FFFF 13.41.37 STC 1455  NSF813I BUSY=0 BUSYSLOT=-1 INFLIGHT=3 ...
```

### What was read, during the stall

**(1) `g_wake_ecb` itself — read during a stall for the first time.** 64-0b could only read
the *pointer* (`anchor+X'24'`), because the word lives at virtual `0BCF94` in **NSFS's
private storage**; a `/.dm` read of that address returns HTTPD's bytes (it read `ABCA0A9A`
in an early pass here, and that value is meaningless). Through the real path:

```
WAKE   g_wake_ecb@0BCF94 real=7BEF94 = 40000000  POSTED=Y
```

`POSTED=Y`, confirmed **during** the stall and agreeing with `NSF812I`'s own `WAKEECB`.
POSTED-bit test only, never a non-zero test. On one pass of stall 2 the page was paged out
and the tool said so — *"page not mapped (pageable, PTE invalid) -- NOT READ"* — rather than
substituting anything.

**(2) The PSW of both CPs.** Both `070E0000 00000000`, **`State: Enabled Wait`**, on every
sample of every pass; `qproc` 000–003 %, 0.20–4.33 MIPS. **MVS had no ready work at all** —
so the executive was not losing a race with anything.

**(3) The executive's task and RB.** TCB3 = `NSFS`. **Byte-identical across three passes
spanning 70 s**, and again at 20:39 — 11 minutes into stall 1:

```
TCB3  9DC7B0 PKF=80 FLGS=0000000001 FLGS4=00 FLGS5=01 OTC=9DE150 RBP=9DCD10
      RB=9DCD10 TYPE=PRB WCF=0 SIZE=17dw INTCOD=0001(SVC 1)
      FLAGS1=02[] STAB2=82[TCBNXT] WCSA=0 CDE=9DCDF8 LINK=9DC7B0
      RBOPSW=078D0000 000D0E72
```

- **`TYPE=PRB`** — the same ordinary PRB REF1 shows. No SVRB, so no supervisor service on
  the stack.
- **`WCF=0`**, `RBXWAIT` **clear**, `RBECBWT` **clear** — the task is **not in a WAIT**.
  REF1 proves what waiting looks like on this stand: `WCF=1`, `XWAIT`, `ECBWT`.
- `RBOPSW` decodes problem state, key 8, **wait bit off**, IA `000D0E72` — inside NSFS's own
  code.
- **`TCBFLGS5=01`** = `TCBPNDSP`, the **primary non-dispatchability** bit.

`TCBPNDSP` is only a summary; `IKJTCB` says the reason lives at decimal 173/174/175/200/201.
Computed (`CBOFF4`, `JOB02231`, CC 0000): `TCBSCNDY`=`TCBNDSP0` at TCB+X'AC',
**`TCBNDSP2` at X'AE'**, and **`TCBNDTS` = X'10'** — *"TASK IS NON-DISPATCHABLE BECAUSE IT
IS BEING SWAPPED OUT"*. Read on every task of the address space **except the region control
task**:

```
TCB0 RCT         TCBSCNDY=00000000   TCBFLGS=0000800000
TCB1             TCBSCNDY=00001000   TCB2  TCBSCNDY=00001000
TCB3 NSFS exec   TCBSCNDY=00001000   TCB4  TCBSCNDY=00001000
TCB5 CTHREAD     TCBSCNDY=00001000
```

#### And the one thing that keeps `TCBNDTS` from being the answer

`TCBSCNDY` was read per-task **once** during stall 1, at 20:41:11. It was re-read at
**20:42:53** and again at **20:46:43** and still read **`00001000`**, with `TCBFLGS5` still
`01` at 20:43:27 — and the reply to the confirming MODIFY landed at **20:41:37**, between
those reads. So the bit was still set while the executive demonstrably ran a pass and
answered.

**A bit that is set while the task is running cannot, by itself, be what prevented it from
running.** `TCBNDTS` is therefore recorded as **correlated with the stall and absent in
three controls, but not established as the blocking cause.** Either it is cleared and
re-set inside a window finer than these samples, or it is a marking that outlives the
condition it names and something else gates dispatch. This round does not separate those,
and the sampling was too coarse to try.

Nothing else in this section depends on it: the PRB, `WCF=0`, the clear `RBXWAIT`/`RBECBWT`,
both CPs in enabled wait and the bit-identical EJST are direct readings of the executive's
own state.

**(4) The anchor.** `inflight=3`; **`SLOT1` and `SLOT2` both `state=1` (PENDING)** with
`reply_ecb=809DE5F0` (WAIT bit set, POST bit clear) and `req_asid` `0007` / `0008` — two
clients in two address spaces parked on requests the STC is not picking up. `BUSY=0`
throughout, as in 64-0b. `served` frozen. EJST bit-identical for over eleven minutes.

### Stall 2 — same signature, independently

MODIFY latency `13.44.02` → `13.47.12` = **3 m 10 s** (again a lower bound; a manual
MODIFY at `13.43.30` waited 3 m 42 s), ended by a host ping. `inflight=3`, `SLOT1`/`SLOT2`
PENDING, `POSTED=Y`, both CPs Enabled Wait, and TCB3 again `RBP=9DCD10` with
`TCBSCNDY=00001000`.

### Two controls, because the bit means nothing without them

| | `TCBFLGS5` | `TCBSCNDY` |
|---|---|---|
| **REF1** NSFS fresh, blocking | `00` | — (summary clear) |
| **REF2** NSFS spinning, no stall | `00` | — (summary clear) |
| **NSFS stalled** | **`01`** | **`00001000`** |
| **HTTPD**, healthy, serving these very reads (8 TCBs) | `00` | `00000000` |

So the bit is **not** a property of this stand, and **not** a property of the spin — REF2
was spinning at a full core with the summary bit clear. It appears with the stall.

### And the address space is NOT swapped out

`D A,L` taken during the live stall:

```
NSFS     NSFS     NSFS      V=V
TSO      TSO      STEP1     V=V  S
UFSD     UFSD     UFSD      V=V  S
TSTRQXCA A                  V=V  S
TSTRQXCB B                  V=V  S
```

NSFS carries **no `S`** — while the two parked gate clients do. And independently: a
**fresh** read of the segment and page tables during the stall reported **every** LSQA page
`VALID`, so NSFS's private storage is resident. The tasks are marked *being swapped out*
while the address space is demonstrably still in.

(64-0's inference that "no `S` flag ⇒ the address space is non-swappable, so swap-out is
excluded" does not survive this: the flag reports *currently swapped out*, and the swap
machinery is visibly involved in a state the flag does not show. `ASCBSWCT`, incidentally,
is **not** a swap counter — `IHAASCB` calls it *"number of times memory enters short
wait"*; it moved 12497→12527 across the round and a first draft of this section misread it.)

---

## 7. The three predictions, as written, and which the run supports

> **S(A)** — the current RB is an SVRB. The task is inside a supervisor service.

**REFUTED.** The current RB is the ordinary PRB, the same one the blocking reference shows,
and its saved PSW is problem-state NSFS code. Nothing is on the stack above it. The device
path — the leading candidate, because both 64-0b stalls ended on inbound traffic, and both
of this round's did too — is **not** where the task is.

> **S(B)** — the current RB is a PRB with a non-zero wait count, while `g_wake_ecb` reads
> POSTED at the same instant.

**REFUTED, and precisely.** Both halves of the setup hold — it *is* the PRB, and
`g_wake_ecb` *does* read `POSTED=Y` at the same instant, now measured rather than inferred —
but **`WCF=0`** with `RBXWAIT` and `RBECBWT` clear. The task is not in a WAIT at all, so
there is no committed WAIT for a POST to have failed to break. **The next investigation is
therefore *not* `DOPOST`'s branch-entry POST.**

> **S(C)** — the task is dispatchable and simply not dispatched.

**Half right, and the half that is wrong is the informative one.** Both CPs sit in the
enabled-wait PSW, so nothing was competing for a processor — but the task is not
*dispatchable*: `TCBPNDSP` is set.

**None of the three fits**, which is now the third round running, and again it is the most
useful outcome. The measured shape is a fourth:

> **The executive is neither running, nor waiting, nor inside a supervisor service, and no
> processor is busy with anything else. It is marked NON-DISPATCHABLE by MVS — `TCBPNDSP`
> set, with `TCBNDTS`, the swap-out indicator, as the reason bit — while the address space
> is still resident.**

The first sentence is measured outright. The second is measured **as a reading** and is
clean against three controls, but its **causal role is open**: the bit persisted across the
wake (§6), so it is correlated with the stall rather than shown to gate it. The refutations
of S(A), S(B) and S(C) do not rest on it.

---

## 8. What is excluded, what survives, and the next question

**Excluded by measurement.** Running (EJST bit-identical, both CPs in enabled wait); the
drain's in-service slot (`BUSY=0`, again); a wake that never arrived (`POSTED=Y`, now read
during the stall, not inferred); a committed WAIT (`WCF=0`); a supervisor service (PRB);
competition for a processor (both CPs idle); and — for this stand and these two
reproductions — swap-out as `D A,L` reports it, with every LSQA page valid.

**What survives** is the non-dispatchability marking itself — as the strongest available
lead, not as an established cause — and the next question is
squarely an **SRM** one, not an NSF one: *what marks the address space for swap-out, and why
is the marking still in force minutes later while the address space is resident and idle?*
`ASCBOUCB` (ASCB+X'90', computed in `CBOFF4`) is where SRM's per-address-space state lives;
reading it needs `IRAOUCB` and is the obvious next instrument.

**What this round does NOT establish, and it is the tempting inference:** that the spin
causes the stall. There is a suggestive chain — a loop that never blocks burns a full core
and looks to SRM like a CPU-bound address space doing no I/O — and 64-0b's table shows every
stall to date at `POSTED=Y`. But **REF2 was spinning at a full core with the summary bit
clear and no stall**, so spinning alone is not sufficient, and nothing here tests the
causal step. It is a hypothesis with a mechanism, which is more than it had, and it is
still a hypothesis.

**Consequence for 64-1, stated as narrowly as the evidence allows.** The reset clears
`g_wake_ecb` in `nsfsx_drain` so the loop can block again. This round shows the stall is
**not** in the WAIT/POST path — the task is not waiting — so the reset is not a wake fix
and cannot be justified as one. Its independent justification is unchanged and now sharper:
it removes a permanent full-core spin, which is an ADR-0022 violation on its own terms and
is also the only known way this address space differs from every healthy one on the stand.
Whether removing it also removes the stall stays **untested**, and 64-0b's instruction still
holds — 64-1 must attempt a reproduction *after* the reset, using this round's arm (the
two-client gate, not an idle window) and this round's passive detector.

Phase 1 is not implicated: `nsfreq_drain` resets its ECB, so `nsfmain` never spins. But
`nsfmain` shares `evt_mainloop`, so if the marking turns out to be reachable without the
spin, it is reachable in Phase 1 too.

---

## 9. Round hygiene

Deploy order `P NSFS` → `make deploy` → `S NSFS`; no mid-chain HTTP 500. Deploy-took-effect
confirmed by `NSF813I` carrying `BUSY=` — the field, not the message id. Host suite
**2925 PASS / 0 FAIL** over 27 tests, before and after the change.

`TSTRQXM` batch **CC 0** with the host peer verifying **9353 bytes byte-exact**, and the
listener was verified in `LISTEN` state with a freshly truncated log before the run and its
`mtime` checked after — 64-0b's stale-log catch, applied. `TSTRQXC` **CC 0** batch and TSO.
The gate jobs of rounds 2–5 returned CC 20 / CC 1: they were run as **workload**, not as a
gate, and the stalls disrupted their mutual timeout coordination, which is what CC 20 means.

`IGF991I`/`IGF995I` for device 500 appeared late in the round — the familiar CTCI-pair
degradation after sustained use, not a new symptom.

**Zero dumps.** Every stop clean apart from the one deliberate `NSF054W`. Stand left with
**NSFS stopped** and TESTLIB holding `TSTRQXM` + `TSTRQXC`.

### Cost, on purpose

The `LEAK` induction ran, so the closing `P NSFS` took the retain branch as budgeted:

```
NSF054W 1 CLIENT(S) STILL IN FLIGHT -- CSA AND SVC ROUTINE RETAINED (EXHAUSTED=6011)
```

`NSF055I`'s largest free block: **794 624 at the start of this round → 655 360 after**, i.e.
this round retained **139 264 bytes** (the ~137 KB pool plus the router) until IPL. The
figure to carry into the IPL that stands before (e): the stand began the 64-0 round at
933 888.

---

## 10. What this round did not do

- **No fix**, of any kind. Not the ECB reset, not a floor, not a change to `DOPOST`.
- **No ADR.** Decision 2 remains Mike's.
- It did not read the OUCB, so *why* SRM marked the address space is not established.
- It did not test whether removing the spin removes the stall.
- It did not establish that `TCBNDTS` is what gates dispatch, only that it is present
  during a stall and absent in all three controls — and that it survives the wake.
- **A note for whoever reuses the reader:** it caches the virtual→real page map to
  `/tmp/nsfs-pagemap-*.json` because LSQA is fixed. A cached map reads a swapped-out
  page as *stale but coherent* — the same failure shape as the `/.dm` trap, one level
  down. Re-validate the PTEs, or drop the cache, before trusting a reading.
- It did not reproduce a stall in an idle window, and after four attempts across three
  rounds that now looks like a property of the idle configuration rather than a sampling
  accident.

---

## Appendix — the offset jobs, in full

Kept here rather than in `jcl/` so the diff stays frozen at the instrument repair plus
docs. Each is assemble-only: nothing linked, nothing written. **Keep every SYSIN card
inside column 71** — the first attempt did not, and IFOX00 read the following card as a
continuation and swallowed it, including a macro call (CLAUDE.md §3, in a diagnostic job).

```jcl
//CBOFF    JOB (A),'CB OFFSETS',CLASS=A,MSGCLASS=H,MSGLEVEL=(1,1)
//ASM     EXEC PGM=IFOX00,PARM='NODECK,NOLOAD,NORLD,NOXREF',REGION=4M
//SYSLIB   DD DSN=SYS1.AMODGEN,DISP=SHR
//         DD DSN=SYS1.MACLIB,DISP=SHR
//SYSUT1   DD UNIT=SYSDA,SPACE=(1700,(900,200))
//SYSUT2   DD UNIT=SYSDA,SPACE=(1700,(600,100))
//SYSUT3   DD UNIT=SYSDA,SPACE=(1700,(600,100))
//SYSPRINT DD SYSOUT=*
//SYSPUNCH DD DUMMY
//SYSIN    DD *
CBOFF    CSECT
AASID    DC    AL2(ASCBASID-ASCB)
AASXB    DC    AL2(ASCBASXB-ASCB)
ADSP1    DC    AL2(ASCBDSP1-ASCB)
ACPUS    DC    AL2(ASCBCPUS-ASCB)
XFTCB    DC    AL2(ASXBFTCB-ASXB)
XLTCB    DC    AL2(ASXBLTCB-ASXB)
XTCBS    DC    AL2(ASXBTCBS-ASXB)
TRBP     DC    AL2(TCBRBP-TCB)
TNEXT    DC    AL2(TCBTCB-TCB)
TOTC     DC    AL2(TCBOTC-TCB)
TFLGS    DC    AL2(TCBFLGS-TCB)
TFLGS4   DC    AL2(TCBFLGS4-TCB)
TFLGS5   DC    AL2(TCBFLGS5-TCB)
TPKF     DC    AL2(TCBPKF-TCB)
RBASIC   DC    AL2(RBBASIC-RBPRFX)
RINTCOD  DC    AL2(RBINTCOD-RBPRFX)
RFLAGS1  DC    AL2(RBFLAGS1-RBPRFX)
RWCSA    DC    AL2(RBWCSA-RBPRFX)
RSIZE    DC    AL2(RBSIZE-RBPRFX)
RSTAB1   DC    AL2(RBSTAB1-RBPRFX)
RSTAB2   DC    AL2(RBSTAB2-RBPRFX)
ROPSW    DC    AL2(RBOPSW-RBPRFX)
RWCF     DC    AL2(RBWCF-RBPRFX)
RCDE     DC    AL2(RBCDE-RBPRFX)
REXSAVE  DC    AL2(RBEXSAVE-RBPRFX)
BFTP     DC    X'00',AL1(RBFTP)
BFTSVRB  DC    X'00',AL1(RBFTSVRB)
BXWAIT   DC    X'00',AL1(RBXWAIT)
BECBWT   DC    X'00',AL1(RBECBWT)
         IHAASCB
         IHAASXB
         IKJTCB
         IHARB
         END
/*
```

`CBOFF2` adds `PSAAOLD`/`PSATOLD` (`IHAPSA`) and `CDNAME`/`CDENTPT` (`IHACDE`, which lives
in `SYS1.MACLIB`, not `SYS1.AMODGEN`). `CBOFF3` adds `ASCBEJST`/`ASCBEWST`/`ASCBSWCT`.
`CBOFF4` adds `TCBSCNDY`/`TCBNDSP0..3`, the `TCBNDTS`/`TCBSTPP`/`TCBNDSVC` bit values, and
`ASCBOUCB` — same skeleton, different `DC` lines and macro calls.
