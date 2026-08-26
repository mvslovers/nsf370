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
