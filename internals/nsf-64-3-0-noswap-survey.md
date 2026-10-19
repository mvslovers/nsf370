# Issue #64, step 64-3-0 — what "not swappable" costs, and whether it exists here

**A read-only survey. It builds nothing, changes nothing, and recommends a
route without taking it.** No `SYSEVENT` was issued, no PPT edited, no address
space started or stopped, nothing deployed. Every claim below is either read
live from a named library member on the stand, or read host-side in a named
source file — the two are kept apart throughout, and §8 lists what the probe
does **not** establish.

Round: MVSCE on `mvsdev`, 2026-08-27. NSFS was already up as STC01493 — the
restored `main` build 64-0f left running — and was sampled, never restarted.

**Not read:** `nsf-64-diagnosis-memo.md` is still not in the repo and was not
supplied to this session — the eighth round in a row.

---

## 0. Why the probe, and the one thing it changes

64-0f re-read #64: the address space is **not stuck**. `ASCBSTOR
0FAF3C00 → 0FC26C00` with `OUCBSWC 0 → 1` — one *completed* swap cycle that
took about twelve minutes, during which the executive was non-dispatchable. A
TCP/IP stack other address spaces depend on cannot be unavailable for minutes,
so the question became whether NSFS should be non-swappable at all.

The survey's answer, in one line: **the mechanism exists at this level, NSFS
already holds the state it needs, this project's own direct ancestor did
exactly this for a symptom that reads like #64 — and one question remains
open, which is precisely the question of what it would cost an installation.**

---

## 1. Instruments, and the controls on them

Three read paths, each with a positive control, because an empty result and a
broken query look identical (CLAUDE.md §8.5).

| path | control | result |
|---|---|---|
| PDS member read (`zowe zos-files view`) | `SYS1.MACLIB(WTO)` | read, 6 lines shown |
| member existence (`list all-members --pattern`) | `SYS1.LINKLIB(IEFBR14)`, `SYS1.LPALIB(IEFIIC)`, `SYS1.PARMLIB(IEASYS00)` | all three found |
| live storage (`/.dm`, HTTPD display module) | SVC 239 reads a **distinct** EP from the unused-slot marker — NSFS holds the slot | chase validated |
| ecosystem grep | same grep form for `clib_apf_setup\|SVC 244` | 72 hits |

**A fourth control caught a real failure**, which is the reason it is worth
reporting. Two offsets this survey needed (`ASCBFMCT`, `OUCBWSS`) were not in
any prior round's proved set, so rather than submit an IFOX00 job — which
writes spool, and 64-0e exhausted the spool — the DSECT layouts were computed
from the macro sources by `docs/measurements/64-3-0/cblayout.py`, **gated on
reproducing every offset a prior `CBOFF` job had already proved**:

```
IRAOUCB  17/17 reproduce   -> derived OUCBPSO=0x4c  OUCBWSS=0x4e
IHAASCB  13/13 reproduce   -> derived ASCBFMCT=0x98
```

The `IHAASCB` run **failed first, 0/13**, because that member marks continued
operands with a trailing ` -` the parser did not strip. Had the control not
been there, the survey would have read a wrong offset and reported a confident
number. It is stated here because the gate earning its keep on the first use is
the evidence that it is a gate and not a decoration.

---

## 2. §3.1 — What mechanism exists on this target

### 2.1 The macro exists, and it is not where you would look first

`SYSEVENT` is **not** in `SYS1.MACLIB` (`PDS member not found`, rc 8) — and
neither is `IHAASCB`, which is the tell. It is in **`SYS1.AMODGEN(SYSEVENT)`**,
247 lines, `MACSTAT Z-4 75007/Z40BPSS`.

```
line   4:  &LABEL   SYSEVENT  &EVENT,&ASID=,&ASIDL=,&PGN=,&ENTRY=SVC,&TYPE=
```

### 2.2 The codes, as the macro spells them

56 mnemonics. The four that concern swappability, with their source lines:

| code | mnemonic | line | |
|---|---|---|---|
| 14 | `TRANSWAP` | 37 | make swappable-when-convenient |
| **41** | **`DONTSWAP`** | **91** | **make non-swappable** |
| **42** | **`OKSWAP`** | **93** | **release it again** |
| 43 | `REQSWAP` | 95 | request a swap-out |

The full 56-entry list is in `docs/measurements/64-3-0/sysevent-codes.txt`.
**Both halves of the pair the kickoff asked for exist, and are spelled
`DONTSWAP` and `OKSWAP`** — not `NONSWAP`, not `SETNSWP`.

### 2.3 The entry, and what the macro says about state: **nothing**

```
line 209:  .SVC     SVC   95                SYSTEM RESOURCES MANAGER SVC
line 211:  .BR      ANOP
line 213:           L     15,CVTPTR(,0)        SYSTEM CVT ADDRESS
line 214:           L     15,CVTOPTE-CVT(,15)  SYSTEM RESOURCES MANAGER ENTRY
line 215:           BALR  14,15                INVOKE SYSTEM RESOURCES MANAGER
```

`ENTRY=SVC` (the default) expands to **`SVC 95`**; `ENTRY=BRANCH` expands to a
`CVTOPTE` branch entry (with a `CVTRV609` fast path for the higher codes).

**The macro documents no state requirement anywhere in its 247 lines** — no
prologue, no comment, no `MNOTE`. Its six error messages are all about operand
syntax. That is reported as a finding, not filled in from elsewhere.

So the requirement was sourced two other ways:

**(a) Live, from the SVC table.** Chasing the same path the production STC uses
(`CVTPTR → CVT+X'C8' cvtabend → SCVT+X'84' scvtsvct`, libc370 `cvt.h:193` /
`ihascvt.h:67`) gives `CVT=01D048 SCVT=01D510 SVCTABLE=00FA60`, and entry 95
decodes (`ihascvt.h:89-102`) as:

```
SVC    EPA       TYPE     APF   ESR   NPMT
95     0003CE00  type1    no    no    no     SYSEVENT
244    00EF15F0  type3/4  no    no    no     self-auth (NSF uses today)
239    00A820C8  type3/4  no    no    no     NSF's stolen slot  <- control
255    00BFB000  type3/4  no    no    no     unused slot        <- control
1      0001DEA8  type1    no    no    no     WAIT               <- control
```

**SVC 95 is a type 1 SVC and its `svcapf` bit is OFF** — the SVC FLIH will not
turn an unauthorized caller away. Any authorization check therefore lives
*inside* SRM, which is the significant half of this reading.

**(b) Host-side, from a peer source** — see §3.2, which is where the state
sequence actually comes from.

### 2.4 SRM's own fields say there is an authorization concept

`SYS1.AMODGEN(IRAOUCB)`, the SRM user control block:

(Line numbers throughout are into the captures committed under
`docs/measurements/64-3-0/`: `SYSEVENT-macro.txt`, `IRAOUCB-macro.txt`,
`IHAASCB-macro.txt`, `IEFZB610-macro.txt`, `SGIEF0PT-macro.txt` — so every
quotation below is checkable without the stand.)

```
line 102: OUCBSFL  DC B'10010100' -  SWAPOUT CONTINUATION FLAGS
line 103: OUCBNSW  EQU BIT0 -        NON-SWAPPABLE STATUS
line 120: OUCBAFL  DC B'00000000' -  ALGORITHM STATUS FLAGS
line 127: OUCBASW  EQU BIT7 -        AUTHORIZED FOR DONTSWAP
line 209: OUCBNDS  DC H'1' -         NUM OUTSTANDING DONTSWAPS
line 181: OUCBWSS  DC H'0' -         WORKING SET SIZE AT SWAP-IN
line 180: OUCBPSO  DC H'0' -         PAGES SWAPPED AT LAST SWAP-OUT
```

and `SYS1.AMODGEN(IHAASCB)`:

```
line 117: ASCBNSWP EQU X'01' -  PROGRAM IS NON SWAPPABLE OR
line 118: *                     RUN IN V=R REGION
line 171: ASCBFMCT DS  H -      ALLOCATED PAGE FRAME COUNT
```

Two consequences, both load-bearing:

- **`ASCBNSWP` is a program *attribute*, not SRM's live status** — its own
  comment pairs it with V=R. `OUCBNSW` is the live status. A survey that read
  only `ASCBNSWP` would have reported JES2 and VTAM as swappable, and they are
  not (§3.1).
- **`OUCBASW` — "AUTHORIZED FOR DONTSWAP" — means an authorization concept
  exists.** What grants it is the open question of §6.

### 2.5 The PPT: where it is, and what changing it costs

`SYS1.AMODGEN(SGIEF0PT)` — a **sysgen** macro — generates `IEFSDPPT`:

```
FUNCTION = GENERATE IEFSDPPT, THE PROGRAM PROPERTIES TABLE.
ATTRIBUTES  PAGED LPA, NOT EXECUTABLE
release UZ66634, 08/01/83
```

Entry format is `SYS1.AMODGEN(IEFZB610)` (`PPTLEN` = 16 bytes):

```
line 18: PPTNSWP  EQU X'20' THIS PROGRAM IS TO BE AUTHORIZED TO BE
line 19: *                  NON-SWAPPABLE
```

**Read that wording exactly.** The PPT bit does not *make* a program
non-swappable — it **authorizes it to be**. It is the same word `OUCBASW`
uses. The two fields are describing one mechanism from two sides.

The table holds **14 entries, 9 carrying `PPTNSWP`** (parse verified: 15
`DC C'…'` lines minus the `IEFSDPPT` module-name eyecatcher):

| carries `PPTNSWP` | does not |
|---|---|
| `IEDQTCAM` TCAM, `ISTINM01` **VTAM**, `AHLGTF`/`HHLGTF`/`IHLGTF` GTF, `IEEMB860` **master scheduler**, `HASJES20` **JES2**, `DFSMVRC0` IMS, `IATINTK` JES3 | `IKTCAS00` TSO/SNA, `IEFIIC` initiator, `IEEVMNT2` mount, `IASXWR00` sysout writer, `IRBMFMFC` MF/1 |

**What it costs to change is the part that generalises beyond our machine:**

- There is **no `SCHEDxx`** in `SYS1.PARMLIB` (control: `IEASYS00` found,
  `SCHED*` matched nothing). The PPT is not a parameter on this level.
- `IEFSDPPT` is **not a member** of `SYS1.LPALIB`, `SYS1.LINKLIB`,
  `SYS1.NUCLEUS` or `SYS1.SVCLIB` (controls: `IEFIIC` found in LPALIB,
  `IEFBR14` found in LINKLIB). It is a CSECT link-edited into another module.
- The macro's sysgen parameters (`&PGM`/`&CPU`) add **CPU-affinity entries
  only** — `DC X'00'  SLOT FOR PROPERTIES`, no attribute bits. You cannot add a
  non-swappable program through the documented sysgen input.
- The five spare slots are pre-committed to TCAM's attribute set
  (`PPTSKEY+PPTNSWP+PPTNOPAS`, key 6) with **zero names**, and the `PATCH` area
  sits *after* the `X'FF'` end marker.

So adding a program to the PPT on 3.8j means editing the macro source and
re-assembling/link-editing a system module — a **USERMOD plus an IPL**, for
every installation, not a configuration change.

---

## 3. §3.2 — What the ecosystem does

### 3.1 On the stand: three address spaces, and none of them ours

Read live through `/.dm`, all 11 active address spaces, identity asserted on
every OUCB (`OUCBASCB` must equal the ASCB chased from — the 64-0c LSQA trap);
`docs/measurements/64-3-0/swapsurvey.py`:

```
ASID JOBNAME  ASCB   ASCBFLG1  OUCBSFL   AFL/NDS     FMCT   WSS   PSO ASCBSTOR
1    *MASTER* 01C178 81 NSWP   84 NSW    ASW/2         10     0     0 0FFD7C00  OK
2    JES2     FDE220 80 -      80 NSW    ASW/1         22    12    12 0FE8CC00  OK
3    TSO      FE7D38 80 -      04 -      -  /0          0    31    12 0FBF5C00  OK
4    UFSD     FD0B18 80 -      00 -      -  /0          0    39    12 0FAA5C00  OK
5    NET      FE6820 80 -      80 NSW    ASW/1          7    18    18 0FDE9C00  OK
6    INIT     FE7B58 80 -      04 -      -  /0          0    18    18 0F292C00  OK
7    INIT     FE7980 80 -      04 -      -  /0          0    18    18 0F3A2C00  OK
8    INIT     FD0F18 80 -      04 -      -  /0          0    18    15 0F312C00  OK
9    FTPD     FF9C78 80 -      00 -      -  /0         96    96    75 0F7FBC00  OK
A    HTTPD    FF9748 80 -      00 -      -  /0        180   279   224 0F6A8C00  OK
B    NSFS     FF8B20 80 -      00 -      -  /0         39     0     0 0F995C00  OK
```

**Non-swappable: `*MASTER*`, `JES2`, `NET` (VTAM) — and nothing else.**

**HTTPD is swappable.** So are UFSD, FTPD and NSFS itself. The ecosystem's own
long-running servers all yield under storage pressure; **not one project in
this ecosystem runs non-swappable on this stand.**

Two further readings from the same table:

- **The correspondence is 11/11.** Every address space with `OUCBASW` set is
  running a program that carries `PPTNSWP` in the shipped PPT (`IEEMB860`,
  `HASJES20`, `ISTINM01`), and every address space without it is running one
  that does not (`IKTCAS00`, `IEFIIC`) or is not in the PPT at all
  (HTTPD/UFSD/FTPD/NSFS). This is *consistent with* the running `IEFSDPPT`
  matching the shipped source — it is not a reading of the live table, which
  §8 records as not done.
- **Swapping is live on this stand**, which matters because an all-resident
  sample would prove nothing: TSO, UFSD and the three initiators read
  `FMCT = 0` — swapped out at the instant of sampling.

### 3.1a Over a window: the set is stable, and one server swaps constantly

**12 samples at ~30 s, 21:24:23 → 21:29:53 CEST**
(`docs/measurements/64-3-0/window.log`):

- **The non-swappable set never flickered.** `*MASTER*`, `JES2` and `NET` read
  `OUCBNSW` + `OUCBASW` + `NDS > 0` in **12 of 12** samples; every other address
  space read clear in 12 of 12. So §3.1's table is a steady state, not an
  instant.
- **FTPD swapped repeatedly while doing nothing — 11 distinct `ASCBSTOR` values
  across 12 samples**, its `ASCBFMCT` reading
  `103 103 103 0 0 103 0 0 103 103 103 103`. An idle ecosystem server on this
  stand is swapped out and back every minute or two, and nothing about it is
  unhealthy. **That is the counterweight to pinning anything: swapping is the
  normal condition here, not an exceptional one.**
- HTTPD, NSFS, TSO, UFSD, JES2, NET and `*MASTER*` each held **1 distinct
  `ASCBSTOR`** — resident throughout. NSFS's `ASCBFMCT` sat at **39**, stepping
  to 42 halfway and holding.

**One reading is my own instrument and is discounted.** HTTPD's `ASCBFMCT`
climbed monotonically `187 → 200` across the window — but HTTPD *is* the
instrument (`/.dm` runs inside it), so the survey was inflating the very number
it was reading. HTTPD's frame count is an upper bound on an otherwise-idle
HTTPD, not a measurement of one, and the §4 figure carries the same caveat. The
same trap cost 64-0f a false CPU reading, one layer up.

### 3.2 In the sources: **this project's own ancestor did exactly this**

The ecosystem grep (positive control: 72 hits for a token known present) finds
`SYSEVENT` in exactly one place outside a header comment — and it is
`mvs38j-ip`, the abandoned project NSF is the revival of:

| file | |
|---|---|
| `src/arch/s370/mvsasm/mvsevent.asm` | the seam: `SPKA 0` → `SYSEVENT DONTSWAP` / `SYSEVENT OKSWAP` → `SPKA 8*16` |
| `src/arch/s370/kernel/sysinit.c:67` | `mvsevent(DONTSWAP);` at startup |
| `src/arch/s370/process/xdone.c:54` | `mvsevent(OKSWAP);` at termination |
| `src/arch/s370/jcl/xinu.jcl` | `SETCODE AC(1)` into `SYS2.LOCAL.LINKLIB` |

**The state requirement is SOURCED here, not unknown.** §2.3 records that the
macro documents no requirement in its 247 lines — that is a fact about the
macro, and it is not the same as the requirement being unestablished. The
ancestor states it outright and acts on it:

```
mvsevent.asm:89          SPKA  0                  SYSEVENT requires key 0
```

and the full sequence around it (`sysinit.c:42-67`) is `testauth()` — refuse to
run unless APF-authorized — then `s370state(SUPERVISOR)`, then that `SPKA 0`,
then the SVC, then `SPKA 8*16` back (`mvsevent.asm:107`). **APF-authorized,
supervisor state, key 0 — which is exactly what NSFS already holds** on every
request today (below). The lifecycle is sourced from the same place:
`DONTSWAP` at sysinit (`sysinit.c:67`), `OKSWAP` at termination
(`xdone.c:51-54`).

**Why it is there — and what does and does not transfer.** The ancestor's
comment block (`mvsevent.asm:20-72`) says what `DONTSWAP` was added to fix:

> "MVSDOZE seemed to hang around output message 200 for quite awhile. I once
> waited to see how long and it turned out to be on the order of about
> **25 minutes wallclock**! … My swag is that **SRM is trying to page us out**
> due to what probably seems like a request for a long wait."

**What transfers is the mechanism and the shape**: SRM makes a long-running
server address space unavailable for **minutes**, and `SYSEVENT DONTSWAP` is
the answer that was reached. That is the same mechanism #64 measures — 64-0f's
twelve minutes in `QFL=80[GOO]` — and it is the whole of the precedent.

**The diagnosis does not transfer, and the device parallel carries no weight.**
His trigger is `STIMER WAIT` issued by **MVSDOZE, a governor built against WTO
buffer exhaustion** (`mvsevent.asm:35-45`) — not a device condition. He does
mention MIH complaining about the unsatisfied CTCI read, and in the same breath
discounts it: *"perhaps it's related, perhaps not (probably not)"*. His
RB-chain evidence is `SVC x'2F'` followed by `SVC x'71'`, and his conclusion is
explicitly a guess: *"Disclosure: I didn't actually look at the SRM code to see
that this was really what was happening; it's merely my best guess."*

So this survey claims the **mechanism precedent** and nothing more. An earlier
draft of this section read the parallel as "same OS, same device"; that
overstated it — the author says the device probably is not involved, and
inheriting a diagnosis its own author disclaimed would repeat the mistake this
investigation has already paid for four times.

**How much weight this carries, stated exactly.** It establishes **precedent**
and **the state sequence**. It does **not** establish that no PPT entry was
needed: no `IEFSDPPT` or PPT mention appears anywhere in that repo, but a
Turnkey system's PPT would not be in that repo either way, so the absence is
evidence about the tree and not about the machine it ran on. And "DONTSWAP
seems to solve" is a claim in a comment, not a measurement.

**Two further source facts:**

- **`libc370` has no SYSEVENT service** — the only hit is the field comment
  `cvt.h:433  cvtopte  /* 218 BRANCH ENTRY ADDRESS TO SYSEVENT */`. A seam
  would be new HLASM. It must be **written fresh**: ADR-0005 forbids copying
  the ancestor's code, and this survey cites the precedent, never the source.
- **NSFS already holds the state, and the `NSF` module does not.** NSFS is
  `ac = 1`, calls `clib_apf_setup` (SVC 244 self-authorisation,
  `src/nsfsmain.c:237`) and enters `__super(PSWKEY0, …)` at twelve sites in
  `src/nsfsx.c` — APF-authorized, supervisor state, key 0, on every request
  today. The Phase-1 `NSF` module has **no `ac`, no `clib_apf_setup` and no
  `__super`**: it is unauthorized, problem state, key 8. Its PROC records the
  design point in so many words: *"The STC self-authorises at runtime via
  clib_apf_setup (SVC 244), so NSF.LINKLIB need not be APF."*

**The mitigation is free exactly where the defect lives**, and not free
anywhere else.

---

## 4. §3.3 — What it costs

The field is **`ASCBFMCT`, "ALLOCATED PAGE FRAME COUNT"** (`SYS1.AMODGEN(IHAASCB)`
line 171, offset `X'98'` — derived under the 13/13 control of §1), read live
through `/.dm`.

| | frames | KB | |
|---|---|---|---|
| **NSFS** | **39** | **156** | idle, resident, STC01493 |
| HTTPD | 180–200 | ~720–800 | the comparable long-running server — but it is also the instrument, §3.1a |
| FTPD | 96 | 384 | |
| JES2 | 22 | 88 | non-swappable today |
| VTAM (`NET`) | 7 | 28 | non-swappable today |
| all 11 address spaces | ~355 | ~1420 | private frames only; HTTPD's share is instrument-inflated (§3.1a), so this is not a clean machine-utilisation figure |

**The machine:** `MAINSIZE 16` (MB) = **4096 frames**, `NUMCPU 2`,
`ARCHMODE S/370`, `CPUMODEL 0148` — read from `~/MVSCE/conf/local.cnf`, which
is the file the running `hercules -f conf/local.cnf` names (checked against the
process, not assumed from `custom.cnf` in CLAUDE.md).

**So NSFS pinned would hold 39 frames ≈ 0.95 % of the machine — today, idle.**
Three caveats, because the number is smaller than it will be in practice:

- `ASCBFMCT` counts **private** frames only; NSFS's ~137 KB CSA slot pool is
  common storage and is already permanent, swappable or not.
- It is measured **idle and resident**. HTTPD's 180 resident frames against its
  279-page `OUCBWSS` (working set at swap-in) is the gap between current
  allocation and a working set under load; NSFS under load is higher than 39.
  NSFS's own `OUCBWSS` reads 0 because this STC instance has not yet swapped.
- `REGION=4M` in `SYS2.PROCLIB(NSFS)` is the **virtual** bound, not a
  real-storage claim.

**The consequence, in one sentence:** an address space that cannot yield under
storage pressure does not reduce the machine's demand, it moves it onto
everyone else — so pinning NSFS spends a permanent ~1 % of a 16 MB machine, and
spends it against JES2, VTAM and the master scheduler, which are already
pinned, plus every swappable server that would otherwise have taken the hit.

---

## 5. The predictions, quoted as written

> - **S1** — a self-issued mechanism exists at this level and requires
>   supervisor state, which NSFS can already enter. The mitigation is then
>   self-contained: no sysgen, nothing for an installation to do.
> - **S2** — it does not exist self-issued on 3.8j and only the PPT route
>   works. That makes the mitigation a system-configuration change every nsf370
>   user must replicate, which is a materially worse trade and may change
>   Mike's decision.
> - **S3** — the ecosystem does something else entirely on this stand, in which
>   case that is the pattern and both of the above are beside the point.

**None fires as written**, and each fails differently:

- **S1 — first half CONFIRMED, second half NOT ESTABLISHED.** The mechanism
  exists (`DONTSWAP` 41 / `OKSWAP` 42, `SVC 95`, `svcapf` off), and NSFS
  already has APF authorisation, supervisor state and key 0 — more than the
  prediction assumed. But "nothing for an installation to do" depends on the
  PPT question of §6, which is open.
- **S2 — NOT REFUTED, only unconfirmed.** The self-issued call plainly exists,
  so S2's premise is false as stated; but if the PPT authorisation turns out to
  be required, S2's *cost* is the one that applies.
- **S3 — partly fires, in the strongest available form.** The ecosystem does do
  something, and it is not something else: it is **this project's own direct
  ancestor doing exactly the proposed thing, for a symptom that reads like
  #64**. That makes it precedent rather than an alternative, so it does not put
  S1 and S2 beside the point — it sharpens the choice between them.

---

## 6. The one question left open, and the one measurement that settles it

**Does `SYSEVENT DONTSWAP` require the PPT authorisation?** This is exactly the
S1/S2 boundary and the survey could not close it.

Two readings fit every fact collected:

| reading | fits |
|---|---|
| **`OUCBASW` is granted at ATTACH from `PPTNSWP`** | the two comments use the same word ("authorized"); the 11/11 correspondence |
| **`OUCBASW` is set when the first `DONTSWAP` is accepted** | the same 11/11 correspondence — those three are also exactly the three with `NDS > 0`; `svcapf` off; `OUCBASW` sits in `OUCBAFL`, the *algorithm status* flags, beside runtime bits |

**No discriminating case exists on this stand.** It would take a program
carrying `PPTNSWP` that is running but has *not* issued `DONTSWAP` — and TCAM,
GTF, IMS and JES3 are all absent here. The ancestor cannot settle it either
(§3.2).

**One measurement settles it:** issue `SYSEVENT DONTSWAP` from NSFS — which
already has the state — and read `OUCBNSW`, `OUCBNDS` and `OUCBASW` back
through the instrument this survey already built. If they move, no PPT entry is
needed and the mitigation is self-contained. **That is a change, so it is not
this probe's to make.**

---

## 7. Recommendation, and the trade-off in one paragraph

**Recommended: the self-issued `SYSEVENT DONTSWAP`/`OKSWAP` route, gated on the
§6 measurement — not the PPT.** The two costs are not comparable. A self-issued
call costs an installation *nothing*: NSFS is already APF-authorized in
supervisor state key 0 on every request, `NSF.LINKLIB` need not be APF, and the
whole change is a small HLASM seam (written fresh — `libc370` has none, and
ADR-0005 forbids transcribing the ancestor's) plus `DONTSWAP` at startup and
`OKSWAP` in the same teardown path that already restores the SVC slot. A PPT
entry costs *every* nsf370 installation a USERMOD against a system module and
an IPL, because on 3.8j the PPT is an assembled CSECT with no `SCHEDxx` and no
sysgen input for attributes — and it would make a stack that is currently
installable by copying a PROC and a load library into one that requires
modifying the operating system. If the §6 measurement shows the PPT
authorisation *is* required, that changes the trade materially and the decision
should be re-taken rather than pushed through; and either way the scope
question — NSFS only, where the state is already held and the defect lives, or
also the Phase-1 `NSF` module, which would have to gain self-authorisation it
does not have — is a separate call.

**And it must be written up saying plainly what it is: it cures the symptom
completely and explains nothing.** The twelve minutes remain unexplained, the
measured chain still ends at `QFL=80[GOO]` inside MVS in fields NSF does not
write and cannot see, and **#64 stays open** — the investigation deferred
rather than closed. The ancestor was in exactly this position and said so.

---

## 8. What this probe does NOT establish

- **Whether the PPT authorisation is required** (§6) — the open question, and
  the one that decides what the mitigation costs an installation.
- **That the running `IEFSDPPT` matches `SGIEF0PT`.** The 11/11 correspondence
  is consistent with it; the live table was **not** read. It is not a member of
  LPALIB/LINKLIB/NUCLEUS/SVCLIB, so reading it needs a chase this probe did not
  do.
- **That `DONTSWAP` would fix #64.** Nothing here tests that. The ancestor's
  claim is a comment, and #64's twelve minutes are still unexplained.
- **What NSFS's working set is under load.** 39 frames is idle and resident;
  `OUCBWSS` reads 0 because this instance has not swapped.
- **Anything about how SRM decides to swap.** The survey read fields, not SRM.
- **That being non-swappable has no other cost.** Only the storage cost was
  measured; effects on SRM's domain and dispatching decisions were not.

---

## 9. Acceptance

| # | item | |
|---|---|---|
| 1 | §3.1 answered with member + lines per claim | §2 — `SYS1.AMODGEN(SYSEVENT)` lines 4/37/91/93/95/209/213-215; `IRAOUCB` 102/103/120/127/209; `IHAASCB` 117/171; `SGIEF0PT`; `IEFZB610` 18-19 |
| 2 | §3.2 answered for stand **and** sources, HTTPD stated either way | §3 — **HTTPD is swappable**; non-swappable = `*MASTER*`, JES2, VTAM, stable 12/12 over a window (§3.1a); `mvs38j-ip` issues DONTSWAP/OKSWAP |
| 3 | §3.3 answered with a number and its field | §4 — **39 frames / 156 KB**, `ASCBFMCT` (`IHAASCB` line 171, offset `X'98'`), machine 4096 frames |
| 4 | three predictions quoted, and which fires | §5 — **none as written**, each failing differently |
| 5 | recommendation + trade-off in one paragraph | §7 |
| 6 | diff is docs only | this file + `docs/measurements/64-3-0/` |
| 7 | PR separates live-on-stand from host-side | the PR body does |
| 8 | nothing issued, changed, started or stopped | no `SYSEVENT`, no deploy, no PPT edit, no STC recycle; NSFS sampled as found (STC01493) |
