# Issue #64, step 64-0d — any interrupt, or device traffic specifically?

**Measurement record, not an ADR and not a decision.** 64-0d fixes nothing, and it
proposes nothing. Whether NSF mitigates what is described below is a contract-level
decision for the maintainer, not the evaluator's to make.

Round: MVSCE on `mvsdev`, 2026-08-27, **freshly IPLed**, module built from
`m5-64-0c-suspension-point` and **not rebuilt** — this round changes no production
source, so `NSF.LINKLIB` still holds the binary 64-0c measured (see §9). Console times
are the MVS clock (UTC-5); host times are CEST.

**Not read:** `nsf-64-diagnosis-memo.md` is still not in the repo and was not supplied
to this session. It has now been absent for **four** rounds (64-0, 64-0b, 64-0c, 64-0d);
every reference below is to the three measurement records that *are* in the tree.

---

## 1. Headline

**The executive is not suspended by anything inside NSF, and it is not waiting for an
interrupt in general. Its address space is stuck part-way through an MVS swap-out.**

Read live, during three separate stalls, with a healthy address space read at the same
instant as the control:

| | NSFS, stalled | HTTPD, healthy, same instant |
|---|---|---|
| `OUCBQFL` | **`80` = `OUCBGOO`, "transitioning out of core"** | `00` |
| `OUCBSRC` (swap-out reason code) | **`09`** | `00` |
| `ASCBRCTF` | `00` — `ASCBOUT` clear, **not** swapped out | `00` |
| `ASCBDSP1` | `00` — `ASCBNOQ` clear, still **on** the dispatch queue | `00` |
| every TCB except the RCT | `TCBFLGS5=01`, `TCBSCNDY=00001000` (**`TCBNDTS`**) | `00`, `00000000` |
| `ASCBCPUS` | `0` | `1` |

The swap-out **starts and does not finish**. The address space stays resident — its LSQA
is readable throughout, `D A,L` shows no `S` — while every one of its tasks is marked
non-dispatchable *because it is being swapped out*. That is the state 64-0c measured from
the task side and could not explain; the OUCB names it from the address-space side.

**Three roles, deliberately kept apart, because the round separates them:**

| role | what it is | where it lives |
|---|---|---|
| **suspension** | the stuck swap-out (`OUCBGOO` + `TCBNDTS`) | **MVS/SRM — outside nsf370** |
| **terminator** | completion of the outstanding CTCI read | the device path |
| **provocation** | what makes SRM choose this address space | **untested** — possibly the spin, inside nsf370 |

---

## 2. The IPL and the baselines

Taken first, as instructed. Clean MVS shutdown (`script SCRIPTS/SHUTDOWN.RC` plus the
site STCs by hand), Hercules exited normally, `./start_mvs.sh` → `IPL 150`.

| | largest free CSA block (`NSF055I`) |
|---|---|
| before the IPL, measured this round | **655 360** |
| 64-0c's closing figure | 655 360 — agrees exactly |
| **after the IPL** | **1 073 152** |

The IPL reclaimed the ~418 KB that three rounds of retained anchors had taken, and
restored the stand to the figure it carried at the start of the b4 round. Post-IPL
environment verified before any measurement: `hercifc` running, `tun0` up
(`192.168.200.2 peer 192.168.200.1`), CUU **0500/0501 ONLINE** (`D U` → `CTC O`),
`NSF210I ... MTU 1500`, and a host→guest ping **3/3, 0 % loss**.

---

## 3. The instruments, characterised from primary source before use

Every intervention was dry-run on the healthy machine **before** the IPL, so that
anything one of them damaged was erased by the IPL that followed. That sequencing paid
for itself immediately.

| instrument | what it injects (Hercules source) | what MVS did (measured) |
|---|---|---|
| `ext` | `ext_cmd`, `hscpufun.c:672` — `ON_IC_INTKEY` + `WAKEUP_CPUS_MASK`; a machine **external interrupt**, no device | **`*IEA420A NO FULL CAPABILITY CONSOLES, REASON=EXT`** — it drives MVS **console switching** and degrades the master console until a **second** press yields `IEE143I CONSOLE SWITCH`. Not a free instrument. |
| bare `/` | console input to `0:0009` (`3215-C *syscons cmdpref '/'`) — an **I/O interrupt** on a device unrelated to NSFS, plus the communications task | silent; console remains responsive |
| `i <cuu>` | `i_cmd`, `hscemode.c:2761` → `device_attention(dev, CSW_ATTN)` | `i 0400` → `IEA000I 400,IOE,...` (IOS/ERP path, benign); `i 0500`/`i 0501` → silent |
| host `ping` | real inbound IPv4 on `tun0` → CTCI CUU 0500 | completes the outstanding read |

**One constraint found in the source before the run, and it changed the ladder.**
`device_attention` returns **1 ("busy or interrupt pending")** when `dev->busy`
(`channel.c:4280`), so `i 0500` cannot inject anything while the CTCI read EXCP is
outstanding — which is exactly the stall condition. The own-device rung therefore uses
the **idle half, `0501`**, and `i 0500` is issued anyway because its *rejection* is a
measurement (§5).

---

## 4. Every control-block offset, and where it came from

Computed by **IFOX00 from the `SYS1.AMODGEN` macros themselves** — jobs `CBOFF5`
(`JOB02253`) and `CBOFF6`, both **`NO STATEMENTS FLAGGED IN THIS ASSEMBLY`** — as
`DC AL2(field-origin)` constants read out of the assembly listing. The b2 method, and
64-0c's. Nothing below is from memory or from a web source.

| macro | field | offset |
|---|---|---|
| `IHAASCB` | `ASCBOUCB` / `ASCBOUXB` | `X'90'` / `X'94'` |
| | `ASCBRCTF` / `ASCBFLG1` / `ASCBDSP1` | `X'66'` / `X'67'` / `X'72'` |
| | `ASCBJBNI` / `ASCBJBNS` | `X'AC'` / `X'B0'` |
| `IRAOUCB` | `OUCBQFL` / `OUCBSFL` / `OUCBEFL` / `OUCBUFL` / `OUCBCFL` | `X'10'` / `X'11'` / `X'15'` / `X'17'` / `X'56'` |
| | `OUCBSRC` / `OUCBSWC` / `OUCBASCB` / `OUCBNDS` | `X'25'` / `X'26'` / `X'28'` / `X'84'` |
| | `OUCBFWD` / `OUCBBCK` / `OUCBACT` / `OUCBLEN` | `X'04'` / `X'08'` / `X'50'` / `X'90'` |
| `IHAOUXB` | `OUXBRSW` | `X'0C'` |
| `CVT` / `IHAASVT` | `CVTASVT` / `ASVTMAXU` / `ASVTENTY` / `ASVTAVAI` | `X'22C'` / `X'204'` / `X'210'` / `X'80'` |

Bit values, same source: `OUCBGOO` `X'80'`, `OUCBGOI` `X'40'`, `OUCBGOB` `X'20'`,
`OUCBOFF` `X'08'`, `OUCBOUT` `X'04'`; `OUCBNSW` `X'80'`, `OUCBINV` `X'10'`;
`ASCBTMNO` `X'80'`, `ASCBOUT` `X'04'`, `ASCBNOQ` `X'80'`, `ASCBSTND` `X'04'`,
`ASCBNSWP` `X'01'`.

**Cross-checks, so nothing rests on one derivation.** `ASCBOUCB`, `ASCBDSP1`,
`ASCBRCTF` and `ASCBFLG1` reproduce CBOFF4's independently computed values exactly.
`ASVTMAXU`/`ASVTENTY` reproduce **libc370's `ihaasvt.h`**, which the production STC
already uses. `ASVTMAXU` at `X'204'` rather than `X'4'` is not a slip: `IHAASVT`
generates `DS CL512` before `ASVTBEGN`.

**`OUCBSRC` is UNDECODED.** `IRAOUCB` declares the field and defines no values for it,
and `SYS1.AMODGEN` and `SYS1.MACLIB` contain no other `IRA*` member that does
(`IRAOUCB` and `IRAPMP` are the only two). The stalled NSFS reads **`09`** on every
sample of every stall; the swapped-out client reads **`06`**. Both are recorded as raw
values. **No meaning is inferred for either.**

### Reading another address space, and proving which one it is

The `/.dm` trap 64-0c found is permanent: the display modules run in **HTTPD's**
address space, LSQA is aliased at the same virtual address everywhere, and the wrong
answer looks perfectly healthy. This round's reader (`asread.py`) is keyed by **ASCB**
and proves identity twice — `OUCBASCB` inside the OUCB must equal the ASCB it was
chased from (asserted, printed, `IDENTITY OK` on every reading below), and every private
read goes through **that** ASCB's own `ASCBSTOR` segment table. The page map is rebuilt
**fresh every run and never cached to disk**, which is 64-0c's closing warning applied.

Address spaces were located by name through the **ASVT** (`CVT` → `CVTASVT` → entries →
`ASCB` → `ASCBJBNS`/`ASCBJBNI`), so no ASCB address in this document was guessed.

---

## 5. The three stalls, and the ladder

The arm is 64-0c's: one STC instance, `TSTRQXM` first (**CC 0000**, host peer verifying
its bytes), then the two-client contention gate re-run back to back. **Every stall began
within ~40–90 s of a gate round**, and the detector is passive (`ASCBEJST`, SQA, read by
`/.dm`) — never a MODIFY, which would POST the cib ECB that `nsfsmain.c:316` puts in the
executive's own ECBLIST.

The detector needed one repair before it could be trusted: 64-0c's threshold was
calibrated on the **spinning** state (~1 s/s), and a healthy **blocking** executive
burns only the ~10 Hz heartbeat, so a fresh instance reads as "flat". The criterion used
here is 64-0c's own words — **EJST bit-identical** — which discriminates in both states.

| # | started | ended | duration | how it ended |
|---|---|---|---|---|
| 1 | 10:44:10 | 10:51:19 | **7 m 09 s** | **spontaneously, untouched** |
| 2 | 11:08:40 | 11:27:18 | **18 m 38 s** | **host ping**, after four other interventions did nothing |
| 3 | 11:28:44 | (control) | — | untouched, `tun0` captured |

All three read the identical signature: `QFL=80[GOO]`, `SRC=09`, `RCTF=00`, `DSP1=00`,
`0500 busy`, `0501 not busy`.

### The ladder, in order, each logged as an intervention

Fired into stall 2. Weakest first; each judged by whether `ASCBEJST` moved.

| # | time | intervention | injected? | result |
|---|---|---|---|---|
| 1 | 11:09:26 | bare `/` — console attention on `0009` | yes (**twice** — the standing `hao cmd /` rule echoes it) | **no change** |
| 2 | 11:25:33 | `i 0400` — attention, foreign idle device | yes (`IEA000I 400,IOE`) | **no change** |
| 3 | 11:25:57 | `ext` — external interrupt key | yes (`IEA420A`, then `IEE143I` on the recovery press) | **no change** |
| 4 | 11:26:26 | `i 0501` — **NSFS's own pair**, idle half | yes (`attention request raised`) | **no change** |
| 5 | 11:26:44 | `i 0500` — NSFS's own read half | **NO — `HHC02231E busy or interrupt pending`** | no change; *the rejection is the measurement* |
| 6 | 11:27:14 | host `ping` — real inbound data on 0500 | yes | **ENDED, immediately** |

Rung 5's rejection is independent, hypervisor-side confirmation that the CTCI **read
EXCP was outstanding** for the whole stall. Rung 6's first RTT was **866 ms** against a
normal 0.6 ms — the stack coming back to life — and at that instant **`QFL` and `SRC`
both went to `00`**.

### The machine was never short of interrupts

Measured **during** the stall, which settles the question the round was named for:

- `qproc` across a sampled window: **50–83 % busy, 64–84 MIPS, up to 250 SIOs**. The
  guest was executing continuously and MVS was dispatching other work.
- HTTPD served every one of this round's `/.dm` reads *from inside the guest* throughout
  each stall — dozens per minute, in another address space, while NSFS made zero progress.
- `IGF991I` / `IGF995I` (MIH, "I/O RESTART SCHEDULED FOR DEVICE 500") fired at 3.49.51,
  3.56.08, 4.02.26, 4.11.52, 4.18.10 and 4.24.27 — **three times inside stall 2 alone,
  and the stall continued through all three.** MIH is not the terminator.

### The parked client, read at the same instant — and it is the opposite case

Every stall on record has a client parked with a published request; stall 1 had
`SLOT0 state=1 PENDING`, `reply_ecb=809DE5F0` (WAIT set, POST clear), `req_asid=0007`.
That client's own address space reads:

```
CLIENT7  ASCB=FE7980  RCTF=8E[TMNO,WAIT,OUT,TMLW]  DSP1=80[NOQ]
         OUCB QFL=0C[OFF,OUT]  SRC=06   ASXB unreadable at virt 9DF300
```

Its swap-out **completed**: marked OUT, off the dispatch queue, private storage gone
(the LSQA read fails because the pages are on the page dataset). NSFS's did not. So the
condition is **not** a system-wide SRM decision that happens to catch NSFS — at that
instant SRM was swapping address spaces out perfectly normally, and NSFS is the one that
got stuck doing it.

---

## 6. The three predictions, as written, and which the run supports

> **P(i)** — a console attention ends the stall. The mechanism is "MVS resumes on an
> interrupt and nothing was generating one"; #64 is not a transport defect.

**REFUTED, twice over.** The console attention was fired at T+45 s into stall 2 and the
stall ran a further **17 m 52 s**. And the premise was already false: the machine was at
50–83 % with hundreds of SIOs, and another address space was being dispatched throughout.

> **P(ii)** — the attention does not end it; device traffic does. The condition is
> specific to the device path, and the CTCI/inbound path becomes the subject rather than
> the dispatcher.

**Its ladder prediction is SUPPORTED; its consequence is REFUTED.** Four different
non-device interrupts — including one on NSFS's *own* device pair — did nothing, and
real inbound data ended it at once. But the condition is **not** in the CTCI path: it is
an SRM/dispatcher state (`OUCBGOO` + `TCBNDTS`, both cleared at the exit), and the
inbound read is only what *releases* it. The dispatcher does not stop being the subject.

> **P(iii)** — neither ends it and it resolves on its own; a timer or SRM interval exists
> and the stall is bounded.

**Observed once (stall 1, 7 m 09 s), and its explanation does not hold.** The obvious
terminator — MIH restarting the device I/O — is ruled out: MIH fired inside stall 2 three
times without effect. What ended stall 1 is **unidentified**, and stall 2 shows the
condition surviving ≥ 18 m 38 s, so "bounded" is not established.

**None of the three fits as written** — the third round running that this is the answer,
and again it is the most useful outcome. The measured shape is a fourth:

> **The address space is stuck part-way through an MVS swap-out that started and cannot
> finish. No interrupt as such releases it — not a console attention, not a foreign
> device, not the external interrupt key, not an attention on its own device pair. What
> releases it is the completion of the outstanding CTCI read, and when that happens the
> swap-out state clears to zero in the same instant.**

---

## 7. Is the mechanism inside nsf370 or outside it?

**The suspension is outside nsf370.** It is an MVS/SRM state, in fields NSF does not
write and cannot see, and NSF's own code is nowhere on the stack: 64-0c measured the PRB
and the clear `RBXWAIT`/`RBECBWT`, and this round measured `ASCBCPUS=0` with the
address space marked mid-swap-out. No change to the transport, the ECB, the drain or the
timer can remove it, and 64-1's reset — still justified on its own footing — cannot be
sold as a fix for it.

**What is inside nsf370 is the provocation, and it is untested.** SRM chose *this*
address space; the client next door was swapped out cleanly at the same moment. The two
things that distinguish NSFS from every healthy address space on the stand are (a) a
loop that never blocks, burning a full core (the ADR-0022 violation 64-1 removes), and
(b) an EXCP that is outstanding indefinitely by design, on a link that may be silent for
minutes. Neither is shown to provoke the swap-out.

**The next question under each answer:**

- *If the provocation is the spin* — then 64-1 is the experiment, and it must attempt a
  reproduction after the reset with **this** round's arm and detector. That is 64-0b's
  standing instruction and it is now sharper, because there is a named state to look for
  (`OUCBQFL`) rather than only a symptom.
- *If the provocation is the indefinitely-outstanding read* — then the question is
  whether an address space can hold a read armed for minutes without SRM trying to swap
  it, and the instrument is a comparison this stand already has: **NSFV**, the probe STC,
  which runs the same transport with **no device at all**. If NSFV stalls, the read is
  exonerated; if it never does, the read is implicated. That is a cheap next round and it
  needs no code change.
- *Either way* — whether NSF should mitigate an MVS condition, and how (`SYSEVENT
  DONTSWAP` is the obvious candidate and is a privileged, contract-level change), is the
  maintainer's decision, not the evaluator's.

---

## 8. What this round does NOT establish

- **`0500 busy` is a constant of the configuration, not a variable that was varied.**
  The outstanding read was present in every stall, but nothing here shows it is
  *necessary*. The NSFV comparison in §7 is the instrument that would decide it.
- **The ladder is n=1 per rung, all six fired into one stall, with the ping last.** The
  alternative reading — that stall 2 was going to end at ~1118 s regardless — is weak
  (the effect was immediate, the first RTT was 866 ms, `QFL`/`SRC` went to zero at that
  instant, and a ping has now ended a stall in every round that tried one) but it is not
  excluded by this round's design. The clean form for any further stall is one rung, then
  the ping as a closing positive control in the same stall.
- **The untreated durations are durations-until-something-arrived**, not a property of
  the stall. Stall 1's terminator is unidentified; ambient inbound traffic cannot be
  excluded for it, because `tun0` was not captured until stall 3.
- **`OUCBSRC` 09 / 06 are undecoded** (§4). Nothing is inferred from them.
- It did not test whether removing the spin removes the stall — that is 64-1's, and it
  becomes untestable the moment the reset lands.
- It did not read the OUCB's swap chain (`OUCBFWD`/`OUCBBCK`/`OUCBACT` were captured but
  not interpreted), so *which* SRM queue the address space is parked on is unread.

---

## 9. Round hygiene

- **No deploy, and therefore no deploy hazard.** This round changes no production source;
  `NSF.LINKLIB` still holds 64-0c's module. Confirmed by the *field*, not the message id:
  `NSF813I BUSY=0 BUSYSLOT=-1 INFLIGHT=0 ...` and `NSF812I ... WPREG=Y` on a fresh
  instance. This removes CLAUDE.md §5's most expensive failure class from the round
  entirely.
- **Host suite `make test-host`: 2925 PASS / 0 FAIL, 27 tests** — no-regression only, and
  evidence of nothing else: the diff is documentation.
- **After an IPL on this stand, the Hercules HTTP console and UFSD/FTPD/HTTPD do not come
  up automatically** — the maintainer started them by hand this round. Worth budgeting
  for; `/.dm` is unavailable until HTTPD is up, and every instrument in §4 depends on it.
- `ext` costs a console switch and a second press (§3). Do not treat it as free.
- **Zero dumps.** `TSTRQXM` CC 0000; the gate jobs were run as *workload*, not as a gate.
- The `IEA000I 400` line in the console log at 11:25:33 is this round's rung 2, not a
  fault.

## Appendix — the offset jobs

`CBOFF5` and `CBOFF6` follow the `CBOFF`…`CBOFF4` skeleton in 64-0c's appendix exactly
(assemble-only, `PGM=IFOX00`, `NODECK,NOLOAD`, nothing linked, nothing written), with
different `DC` lines and macro calls: `CBOFF5` calls `IHAASCB`/`IRAOUCB`/`IHAOUXB`,
`CBOFF6` calls `CVT DSECT=YES`/`IHAASVT`/`IHAASCB`. Both are preserved under
`docs/measurements/64-0d/`. **Keep every SYSIN card inside column 71** — the rule that
bit 64-0c in a diagnostic job, and that was checked mechanically before submission here.
