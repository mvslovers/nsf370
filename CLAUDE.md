# CLAUDE.md — NSF (mvs38j-ip revival)

> Operational guide for any Claude Code session working in this repository.
> **Read this file first, then the relevant chapter of the Architecture
> Specification and any ADRs it references, before writing code.**

NSF ("Network Services Facility") is an **event-driven networking subsystem
for MVS 3.8j** whose first responsibility is a native TCP/IP stack. It is a
from-scratch revival of the abandoned `mvs38j-ip` project — **not** a port of
its Xinu code (see ADR-0005). Goal: run existing applications (HTTPD, mvsMF)
**unchanged, relink-only** over a native stack instead of the Hercules X'75'
hack, with full EZASOKET / PROFILE.TCPIP compatibility.

---

## 1. Sources of Truth

Consult in this order. Do not contradict a higher row without an ADR.

| Document | Role | Status |
|---|---|---|
| `internals/Project-Brief-v2.md` | Why / scope / constraints / milestones | **Frozen** — do not expand |
| `internals/Architecture-Specification.md` | How: interfaces, data structures, lifetimes | Living, versioned |
| `internals/adr/ADR-*.md` | Rationale for individual decisions | Append-only |
| `CLAUDE.md` (this file) | Operating rules + one-line status | Update on convention changes; **keep under 40k characters** |
| `internals/milestone-history.md` | The per-round record behind §7 | Append-only |
| `internals/m5-status.md` | M5 status and open items (moved out of §7) | Update with each M5 round |
| `TODO.md` | Open work, ranked, with the reason for each rank | Update after every commit, push or merge that moves an item |
| `internals/assembler-rules.md`, `internals/mvsdev-runbook.md` | The full record behind §3's HLASM rules; the live-target runbook (short form first) | Append; keep §3 in step with its record |
| Notion → *Issues & Tasks*, project **mvs38j-ip (NSF)** | Task tracking (`[NSF] Mx-y`) | Mirrors §7 below |

**Read when relevant** — not by default:

| Before you … | read |
|---|---|
| change or review any `asm/*.asm` | `internals/assembler-rules.md` |
| deploy, run `mbt test --mvs`, drive the console, touch Hercules/CTCI on `mvsdev` | `internals/mvsdev-runbook.md` |
| work on M5 or pick up an open M5 item | `internals/m5-status.md`, then `TODO.md` |
| cite or build on an earlier round's result | `internals/milestone-history.md`, `internals/measurements/` |
| cut a release or touch `[distribution]` / the FMID | `~/repos/mvs/.workspace/context/smp.md` |

Reference material only (**never copy code from these**): the old
`mvslovers/mvs38j-ip` Xinu tree; Comer, *Internetworking with TCP/IP Vol. 2*;
IBM *IP Configuration Reference* (EZASOKET / PROFILE.TCPIP); relevant RFCs
(791, 792, 793, 768, 1071, 1122).

---

## 2. Architecture in One Screen

One executive task runs a single event loop, run-to-completion, no preemption
inside the stack. Async MVS exits (device I/O, timer) do the minimum — enqueue
a pre-allocated element (CS) and POST an ECB — everything real happens later on
the executive task.

```
Applications ─ EZASOKET (NSFEZA) ─ Request Mgr (NSFREQ) ─ Sockets (NSFSOC)
                                                          │
                         TCP (NSFTCP) · UDP (NSFUDP) · IPv4/ICMP (NSFIP/NSFICM)
                                                          │
                    Device abstraction (NSFDEV) ─ CTCI / LCS / HOST drivers
   Foundation (used by all): NSFMM NSFBUF NSFQUE NSFTMR NSFEVT NSFTRC NSFSTS
```

**Two phases, one contract.** Phase 1: stack runs in-process (ATTACHed
subtask) — trivial debugging. Phase 2: own STC as subsystem `NSFS`
(SSI/cross-memory). The `NSFRQE` request block is the phase boundary; only the
transport changes, never the request format.

---

## 3. Non-Negotiable Invariants

These are the rules that keep the system correct on a 24-bit, 16 MB machine.
Violating one is a review-blocking defect, not a style nit.

**Memory**
- Protocol code never calls `GETMAIN` / `malloc` / any storage service.
  **Only `NSFMM` owns storage.** `mm_pool_create` is init-time only (it
  ABENDs if called later — the rule is enforced, not documented).
- `mm_alloc()` returning `NULL` is **normal and expected**. Every caller
  handles it gracefully (drop packet + count, reject connection, fail API with
  `ENOBUFS`). Exhaustion is never an ABEND.
- Every control block declares its byte size and is guarded by
  `NSF_SIZE_ASSERT(type, size)`.

**Buffers (PBUF)**
- **Single owner, no reference counting.** A function either *keeps* a PBUF or
  *passes it on* — never both, never neither. Each interface states which.
- **Only the executive task frees buffers.** Exits queue them for freeing.

**Execution**
- Event handlers **never** WAIT, **never** loop over unbounded input, **never**
  call `mm_pool_create`. Run-to-completion. The loop enforces a drain budget so
  a flood cannot starve timers.
- All mainline state is single-task — **no locking** except the CS-based
  exit→mainline handoff (`xq_push`) and POST.
- Queues are **bounded** by default (socket rx, listen backlog, device send);
  reject rather than grow.

**Timers**
- `TMR` structures are **embedded** in their owner CB (never allocated).
  Arming cannot fail.
- `tmr_cancel` is idempotent and **mandatory in every teardown path**.
- **Arming/consumption contract (ADR-0034):** NSFTMR owns the `nsfstim.h` seam
  through one word (`g_armed`) equal to what the `STIMER` is armed for, so the
  invariant **queue empty ⟺ `STIMER` disarmed ⟺ `g_armed == 0`** holds — enforced
  at BOTH drain points (`nsftmr_run` on a fire, `tmr_cancel` on a cancel). The
  executive advances the queue via **`nsftmr_wake()`** (the ARMED tick count),
  never `nsftmr_run(1u)` per wake — that was issue #40 (a delta-N timer fired
  after N(N+1)/2 ticks). `tmr_start` arms whenever the inserted timer becomes the
  head (bootstrap + head-shortening). A green host build does **not** prove
  cadence: the host has no `STIMER`, so tests call `nsftmr_run`/`nsftmr_wake`
  directly — the live cadence gate is `test/mvs/tsttmcad.c`.

**Lifetime / teardown**
- Every object dies through **exactly one** destroy function with a full
  checklist (`soc_destroy`, `tcp_destroy`, device quiesce). Never tear down ad
  hoc. This is the direct defense against the resource-leak / secondary-ABEND
  class of bug.

**Recovery**
- The executive task runs under **ESTAE from M0 onward**. The recovery path
  releases **only what nobody else will** (ADR-0049): the SVC slot and the CSA
  anchor, never the private-area pools or the devices. A FREEMAIN under a
  parked EXCP abends A0A-2 (#83), and quiescing a device is blocking work an
  exit must not do. A crash must never require a Hercules restart to clean up.

**Compatibility & scope**
- Sockets are **binary-transparent**: NSF never converts payload between EBCDIC
  and ASCII (application's job, like IBM). NSF only converts its own text
  (config, operator messages, trace, later DNS names).
- **v1 does not fragment or reassemble IP.** Inbound fragments are dropped and
  counted; transports respect MTU (TCP MSS, UDP `EMSGSIZE`).
- **No Xinu code.** Fresh implementation from RFC/IBM docs.

**External symbols**
- cc370/ld370 fold every external name to **8 characters** after upcasing and
  mapping `_` → `@`. On a collision ld370 keeps one definition **silently**, so
  two C functions that agree in the first 8 mangled characters bind to the *same*
  code — a wrong-function dispatch that only bites on MVS (native host builds
  have no 8-char limit and never see it; e.g. `buf_trim_head`/`buf_trim_tail` →
  `BUF@TRIM`, `nsf_abend`/`nsf_abend_sethook` → `NSF@ABEN`).
- Therefore **every cross-module (non-`static`) NSF C function carries an
  explicit 8-char uppercase `asm("XXXXXXXX")` alias on its declaration in the
  header**, unique across the whole load module, on a per-component scheme
  (`NSFB*` buffers, `NSFM*` memory, `NSFQ*` queue, `NSFTR*` trace, `NSFST*`
  stats, `NSFA*` abend, `NSFX*` xq, plus `NSFNOW`/`NSFTASK` for the time seam).
  Each header lists its aliases in a comment block. **Never rely on cc370 name
  truncation.**
- At every **C↔asm boundary** the asm `CSECT`/`ENTRY` name must equal the C
  alias character-for-character (`xq_push` `asm("NSFXPUSH")` ⇄ `NSFXPUSH CSECT`),
  so resolution never depends on the mangling rule.
- **Reviewer checklist (assembler/C):** a new non-static header function has a
  unique `asm()` alias; a new or renamed asm CSECT matches its C alias; a green
  host build is **not** evidence here — confirm no duplicate `PDPPRLG`/`ENTRY`
  symbols in the `cc370 -S` output (or a clean on-MVS link).

**C-callable HLASM (entry convention)** — short form; the measurements and
incidents behind every rule are in **`internals/assembler-rules.md`** (read it before
changing any `asm/*.asm`).
- Every C-callable HLASM routine is built the **standard cc370 way** — `COPY
  MVSMACS` + `COPY PDPTOP`, `FUNHEAD` prologue, `FUNEXIT` epilogue (`FUNEXIT
  RC=(Rn)` when it returns a value in R15) — modeled on libc370
  `asm/@@getclk.asm`. **Never hand-roll `STM`/`BALR`/`USING`:** it omits what
  the cc370 C runtime (`@@CRTGET`) relies on and ABENDs the *next* C library call
  (S0C6) while linking and host-testing clean (issue #8).
- A **leaf** routine (issues no SVC/macro, calls nothing) uses the plain
  `FUNHEAD ,` form (like `nsf_now`). A routine that **issues an OS macro/SVC**
  (OPEN/CLOSE/EXCP/WTO/…) must give its callee a save area: use `FUNHEAD
  SAVE=name,US=NO` with a static `name DC 18F'0'` in the CSECT (single-task, so
  one shared static area is safe, per `nsfctcio.asm`) — **not** the leaf form.
  (64-3-1 found this too broad: what decides is the SVC *type* — a type-1 SVC
  such as STIMER or SYSEVENT saves into the SVRB, so `NSFTMARM`/`nsfsevt.asm`
  are leaves; see the record.)
- The `FUNHEAD` entry name IS the 8-char `asm()` alias, character for character.
- Address static data by **explicit displacement** `LABEL-entry(,R12)` (or a
  register), never a bare-label `USING` (as370 drops those to base 0 — S102).
- Address a **DSECT field** as a difference expression `FIELD-<origin>(Rn)`,
  never a bare `FIELD(Rn)` (as370 subtracts the active USING base — cc370 #18).
- Keep `CS`/`LM` (RS-format) operands `D(B)`, never `D(,B)` (#5).
- **NO card of any kind — instruction OR whole-line `*` comment — may reach
  column 72.** Column 72 is the continuation flag; an overlong card **eats the
  next card**, silently dropping an operand or a whole statement (the M3-0b
  `SVC 33`; `nsfctcio.asm`'s swallowed `DCBD`; four safety cards lost in
  `nsfvsvc.asm`). Chains can be longer than one card. as370 now raises a
  swallowed *statement* to **RC 8** (cc370#84); a swallowed *comment* is
  severity 4, and IFOX does not warn at all when the eaten card's columns 1-15
  are blank. `tools/check-card-columns.sh` (CI `asm-cards`) counts **bytes**
  (a UTF-8 `§` costs two).
- **Object comparison:** the END card carries the assembly Julian day at column
  52 of the last card — mask that one byte before reading a diff as a code
  change.
- **`as370` knows only RR/RX/RS/SI/SS plus `IPK`/`SPKA`/`STCK` — no SSE, no
  RRE.** Anything else is raw `DC X'…'` with the encoding from **primary source**
  (Hercules `opcode.c` for this target). A known mnemonic can still assemble
  **wrong**: `MVCK` dropped its `R1`/`R3` registers before as370 1.6.3, so it
  was raw `D9` (≤255-byte pieces, ADR-0039). Since #129 `nsfvsvc.asm` uses the
  mnemonic, proven byte-identical; `test/mvs/tstmvck.c` still emits raw bytes.
  Unknown ones (`SSK`/`ISK`,
  `TPROT`, `MVCDK`/`MVCSK` — the last two absent on this target, `S0C1`) cannot
  assemble at all. **The `as370 -a=` listing is the gate on the emitted bytes —
  never a green link.**
- **`LRA`+`SSK` on a pageable frame is unreliable** (MVS can rebind the page
  between the two), but the error is **false-negative only** for tests that
  expect a fault — a PASS is trustworthy; prefer a `GETMAIN SP=241` (key 0 by
  allocation) destination.
- **Exception:** a routine the OS invokes as an *exit* (not called from C) is not
  a C callee and does not get `FUNHEAD` — e.g. `NSFTMEXP`, the STIMER exit.
- **Reviewer checklist (assembler):** a new C-callable routine uses
  `FUNHEAD`/`FUNEXIT`, not hand-rolled; its entry name matches its `asm()` alias;
  data addressed by explicit displacement; `CS`/`LM` stay `D(B)`; nothing past
  column 71; every source card present in the `as370 -a=` listing in order. A
  green host build and a clean link are **not** evidence — these failures are
  MVS-runtime only.

**Standard library truncation (libc370, ADR-0026)**
- **Never call `vsnprintf`/`snprintf` directly** (libc370 1.0.2 and earlier did
  not NUL-terminate on truncation, issue #25; the history is in ADR-0026) — use
  `nsf_vsnprintf`/`nsf_snprintf` (`include/nsffmt.h`), which always
  NUL-terminate when `size > 0` and return the count of characters actually in
  the buffer (clamped), not the raw C99 value.

**Contracts**
- `NSFRQE` (the app↔stack request block) **freezes at the M3 exit gate**.
  Changing it afterwards requires an ADR.
- **`ubuf` and `ulen` are transport-owned (ADR-0047):** `ubuf` is an address in
  the caller's address space and `ulen` is a length in **bytes, for every verb**.
  A verb-specific meaning belongs in `sockdesc`/`p1`/`p2`/`p3`, which the
  transport never interprets (the **dual of ADR-0003**; the transport moves
  `min(ulen, 2048)` bytes and never reads `fn`). When the two sides of such a
  pair are in **separately linked load modules** — the facade in the
  application, the engine in the STC — a host test cannot see a disagreement;
  that is what an `NSF_SIZE_ASSERT` on the element is for (`RQ_SELECT`, #101,
  the record is in ADR-0047).

---

## 4. Naming Conventions

- **Component prefix:** `NSF`. **Subsystem (Phase 2):** `NSFS`.
- **Load modules / CSECTs:** `NSF` + up to 5 chars — `NSFMM`, `NSFBUF`,
  `NSFTCP`, `NSFCTCI`, …
- **Source files:** lowercase — `nsfmm.c`, `nsftcp.c`, `nsfctci.asm`,
  `nsfxq.asm`. One header per component in `include/`.
- **External C symbols:** every cross-module C function carries a unique 8-char
  `asm()` alias in its header, and asm CSECT names match that alias — see §3,
  "External symbols" (cc370 truncates externals to 8 chars).
- **Message IDs:** `NSFnnns` = `NSF` + 3-digit number + severity
  (`I`/`W`/`E`/`S`), e.g. `NSF001I NSF INITIALIZATION COMPLETE`.
  Number ranges: 000–099 executive · 100–199 memory/buffers · 200–299 devices
  · 300–399 IP/ICMP · 400–499 UDP · 500–599 TCP · 600–699 sockets/API ·
  700–799 config · 800–899 operator · 900–999 recovery.
- **External API keeps IBM names** at the boundary (`EZASOKET` entry point,
  PROFILE.TCPIP statement keywords). Compatibility outranks branding there.
- **Timed ECB waits (ADR-0025):** `ecb_timed_waitlist`'s timeout ECB wakes the
  WAIT only if it is **in** the waitlist — `nsfthr_timed_wait`/`nsfthr_join`
  wait on `{target, tmo|VL}`. Never test an ECB word for non-zero (a satisfied
  multi-ECB WAIT leaves an RB-address remnant in un-posted ECBs); test the
  POSTED bit.

---

## 5. Toolchain & Build

**Toolchain (decided — ADR-0013):**
- **C compiler:** `cc370` — complete host cross-compile suite.
- **C runtime:** `libc370` — the `cc370` **sysroot** (target C library),
  provided by the toolchain; **not** a `[dependencies]` entry (ADR-0014).
- **Assembler / linker:** `as370` / `ld370` on the host (mbt 3 runs
  `as370 -o build/x.o asm/x.asm` and `ld370 … -o build/NSF`).
- **Build orchestration:** **mbt 3**, configured by `mbt.toml` at the root
  (`[toolchain]` pins mbt and the release toolchain). `.env.example` stays as
  the template `mbt target import .env --name NAME` reads.

**Two build worlds, both driven by mbt (ADR-0014):**

| World | Builds | Compiler | Needs MVS? | Runs in CI? |
|---|---|---|---|---|
| Host (`mbt test`) | portable C + `nsf*_host.c` shims | native cc | No | **Yes** |
| MVS (`mbt test --mvs`, `mbt deploy`, …) | full stack incl. `asm/*.asm` | cc370 + as370 + ld370 | Yes (live 3.8j over IP) | No |

The host build is configured by `[build.host]` in `mbt.toml`; its `replace`
map swaps each MVS-only CSECT (`asm/*.asm`) for its `src/*_host.c` shim.
Rules: `asm/*.asm` never compiles on host; everything else compiles both
ways; warnings-as-errors everywhere. The distinction between the worlds is
MVS reachability. mbt 3 cross-compiles with `-Wall -Wextra -Werror`.

**Commands (mbt 3; `mbt --help` for the full list):**
```
mbt test                  # native build + run of the portable tests (no MVS)
mbt build --all --tests   # cross-compile every load module and test module
mbt test --mvs [--only X] # build + deploy the test modules, run them on MVS
mbt deploy                # put the load modules into NSF.LINKLIB (members replaced)
mbt package               # release artifacts in dist/
mbt doctor [--offline]    # toolchain, sysroot, MVS connection
```

**C dialect:** `-std=gnu99` (as set in `mbt.toml`), used conservatively —
no VLAs, no runtime allocation, fixed-width via the project typedefs
(`UCHAR/USHORT/UINT/INT`), big-endian S/370, AMODE 24 / RMODE 24, EBCDIC on
target. `cc370` accepted these flags on both host and MVS at M0-1; record any
later `cc370` limit surprises in an ADR. Comments and documentation in
**English**.

### Live MVS target: what runs on `dev1` without asking

Target: **MVSCE on Hercules on `mvsdev`** (`mvsdev.lan`), mbt target `dev1`.
Credentials live in the keychain and the zowe `mvsdev` profile — never hardcode
them.

**Without asking** (the one exception the root `CLAUDE.md`, "Working
autonomously", grants an nsf370 session): its own cycle on `dev1` —
`P NSFS`/`P NSFV`, `mbt deploy --target dev1`, `S NSFS`/`S NSFV`, and
`mbt test --mvs --target dev1`. **Everything else on `dev1` asks first**
(submit, allocate, delete, PARMLIB/PROCLIB, APF, any other STC), and so does
every other stand; a write to `SYS1.*` needs its own approval per task. The
root `CLAUDE.md` is the rule; this paragraph only names nsf370's cycle.

**Before you deploy, run `mbt test --mvs`, drive the console, or touch
Hercules/CTCI, read `internals/mvsdev-runbook.md`** (short form first, then the full
record). The one trap to carry: keep the order **`P NSFS`/`P NSFV` → `mbt
deploy` → `S NSFS`/`S NSFV`**, or the next run may silently test the old binary.

---

## 6. Repository Layout (spec §16.2)

Flat layout (ADR-0014); the `nsf*` prefix namespaces every source, so no
per-layer subdirectories. Components stay grouped by prefix and the §9 map.

```
<repo>/
├── mbt.toml          mbt 3: modules, tests, [build.host], [toolchain], deploy
├── .env.example      MVS connection template for `mbt target import`
├── CLAUDE.md         this file (repo root — NOT under docs/ or internals/)
├── TODO.md           open work, ranked (issues stay the source of truth)
├── src/              portable C: nsfmm.c nsfbuf.c nsfque.c nsftmr.c
│                     nsfevt.c nsftrc.c nsfsts.c nsfip.c nsficmp.c
│                     nsfudp.c nsftcp.c nsfsoc.c nsfreq.c nsfeza.c
│                     nsfdev.c nsfctci.c … + nsf*_host.c host shims
├── asm/              HLASM: nsfctcio.asm nsfxq.asm nsfstim.asm nsftime.asm
│                     nsfvsvc.asm nsfsevt.asm …
│                                          ← ONLY MVS-specific asm.
│                     (WTO / CIB / ESTAE reuse libc370 seams, not hand-rolled
│                      nsfwto.asm/nsfestae.asm — ADR-0018; the async STIMER
│                      exit lives in nsfstim.asm. MVS-only C glue that is NOT
│                      asm — nsfmain.c, nsf*_plat.c — sits in src/.)
├── include/          one header per component (nsf.h + nsf*.h)
├── cfg/              sample PROFILE.TCPIP members
├── jcl/              install/SAMPLIB jobs
├── test/             dual host+MVS tests (tstsmoke.c, …)
│   ├── mvs/          Level 2/3: on-MVS component & integration jobs
│   └── asm/          HLASM test callers
├── docs/             for USERS of NSF
│   └── ezasoket-conformance.md  EZASOKET verbs, return codes, ERRNOs vs IBM
└── internals/        for MAINTAINERS (sessions included)
    ├── Project-Brief-v2.md
    ├── Architecture-Specification.md
    ├── adr/          ADR-0001 …
    ├── milestone-history.md  the per-round record behind CLAUDE.md §7
    ├── assembler-rules.md    the full record behind CLAUDE.md §3 (HLASM)
    ├── mvsdev-runbook.md     the live-target runbook (moved out of CLAUDE.md §5)
    ├── m5-status.md          M5 status + open items (moved out of CLAUDE.md §7)
    ├── measurements/ per-round evidence AND the instruments that took it
    └── procedure-*.md  operator-driven gate procedures (not tests)
```

**`internals/measurements/` holds both evidence and instruments, and that is deliberate.** Logs
are **evidence**: what a round measured, kept so a later claim can be checked against what
was actually seen rather than re-derived. Scripts are **instruments that outlive their
round** — `40-ident/arm1.py`'s `/.dm` reader with its proved control-block offsets,
`m5-2c2/rowwatch.py`, the various drivers — and every one of them has been reused by a later
round than the one that wrote it. Both live under `internals/measurements/<round>/`. Do not move
the scripts to `tools/`: `tools/` is for things the build or CI runs, these are things a
person runs while investigating, and their value is inseparable from the round's write-up
that says what they were pointed at and what the reading meant.

**A procedure is not a measurement and not a test.** `internals/procedure-*.md` is for something
an operator executes at a gate — steps, expected output at each, what a broken result looks
like, and the honest limit that keeps it out of `mbt test --mvs`. Name the coverage kind
wherever it is referenced: **a coverage kind that is not named is read as a test a year
later.**

---

## 7. Milestone Status

Mirrors Notion (*Issues & Tasks*, project *mvs38j-ip (NSF)*). Every milestone's
**Definition of Done** = host Level 0/1 green in CI + the demonstrable
deliverable shown on Hercules via mbt + **leak gate** (all pools back to
baseline in-use after quiesce) + spec/ADRs updated.

**The full per-round record — what each round built, measured live, and did
NOT establish — is `internals/milestone-history.md`.** Keep this section to one
status line per milestone; a round appends its record
there and updates the line here (§8.7). References such as "CLAUDE.md §7,
M5-2b4" in older ADRs point into that file.

| MS | Scope | Exit gate | Status |
|----|-------|-----------|--------|
| **M0** | Foundation: MM, buffers, queues, timers, event loop, trace, stats, config, STC skeleton + ESTAE | `F NSF,DISPLAY,STATS` answers; clean stop, pools at baseline; CI green | ✅ Complete — validated live on TK5 (STC00175) |
| **M1** | CTCI driver (HLASM top / C bottom) + NSFDEV + NSFHOST | ping → hexdump in trace; crafted packet in host `tcpdump` | ✅ Complete — I/O subtasks (ADR-0022/0023), live on MVSCE 0500/0501 |
| **M2** | IPv4 + routing + ICMP echo/errors + checksum | `ping <mvs-ip>` sustained, 0 loss | ✅ Complete — 1000/1000 after #21 pair sequencing (ADR-0025) |
| **M3** | Sockets + NSFRQE + UDP + EZASOKET (M3 set) | UDP echo via EZASOKET; leak-free; **NSFRQE freezes** | ✅ Complete — PR #34; NSFRQE frozen at 64 B; #28 closed (ADR-0030) |
| **M4** | TCP + EZASOKET (M4 set) + loss harness | telnet echo, clean FIN, survives 5 % loss; TIME_WAIT reclaim | ✅ Complete — PR #43; #40 timer cadence fixed (ADR-0034) |
| **M5** | Phase 2: `NSFS` subsystem + cross-memory + TCP hardening + docs | 2 address spaces share one stack; stress passes; docs complete | 🔄 In progress — see below |
| **M6** | *(stretch)* HTTPD + mvsMF on NSF; DNS; LCS + ARP | HTTPD & mvsMF run unchanged (relink) on TK4-/TK5 | ☐ Planned |

**M5 — where it stands, and its open items:** `internals/m5-status.md` (d2, the
stranded retained anchor; c3, retiring the probe verbs; TCP hardening + docs).
**Open issues and their order live in `TODO.md`** (ranked, with the reason for
each rank); the GitHub issues stay the source of truth (`gh issue list`).

---

## 8. Working Agreement (per task)

1. **Read** this file → the spec chapter for the component → referenced ADRs.
   Do not infer behavior the spec fixes; if the spec is silent, propose an ADR.
2. **Host-test first.** Write Level 0/1 tests with the native compiler before
   (or alongside) the implementation. They must stay runnable without MVS.
3. **Respect the invariants in §3.** Especially: no allocation on hot paths,
   single-owner buffers, one destroy function per object, `NSF_SIZE_ASSERT` on
   every CB, ESTAE coverage, and an 8-char `asm()` alias on every cross-module
   function (asm CSECT names match).
4. **On-MVS validation** via `mbt test --mvs` at milestone boundaries and
   before merging anything touching `asm/*.asm`.
5. **Any operation whose ABSENCE is indistinguishable from its SUCCESS needs a
   third state or an assert.** This is the sibling of the project's most
   expensive bug class (host-clean, link-clean, wrong only on the machine) one
   step earlier: *not executed, and nothing says so*. Four sightings in M5-2b
   alone, all the same shape — the instances are the point, the rule alone reads
   as a platitude:
   - a gate that **skipped its load-bearing case and returned CC 0** → fixed
     with a distinct skip code (`XF_CC_GATE_SKIPPED`, `test/mvs/tstrqxf.c`);
     return it **from the test**, never by changing the mbt harness;
   - **`make deploy` (mbt 2) failing mid-chain**, after which the run silently tests the
     previous module (`internals/mvsdev-runbook.md` has the signature and the tell);
   - a **`str.replace` that no-op'd** because a linter had quoted the target
     line — an unasserted edit that reported success having changed nothing;
   - a **self-check word never written** reading the same as one that ran and
     failed → three states (1 pass / 2 ran-and-failed / 0 never-written,
     `asm/nsfvsvc.asm` `DOPOST`).
   The cure is always one of: a third state, an assert that the thing happened,
   or a positive check that the thing took effect. Never infer success from the
   absence of a complaint.
6. **Definition of Done** (§7) must hold, including the leak gate.
7. **Keep docs honest:** when a decision changes, update the affected spec
   chapter and add/append an ADR in the same change. Update the one-line
   status in §7 here (and `internals/m5-status.md`) and **append the round's record to
   `internals/milestone-history.md`** — never grow §7 itself; this file must stay
   under 40k characters (`wc -c CLAUDE.md`).
8. **Ask before changing an Issue's STATE** (`gh issue close` / `reopen` /
   `edit`), because the audit trail cannot tell us apart — every action is
   `mgrossmann` — so a state change nobody intended is unattributable afterwards
   and reads as a decision (issue #64 was closed `COMPLETED` while three places
   in the tree carried it as open).
9. **English** for all code comments, commit messages, and docs.

---

## 9. Quick Module Map

| Prefix | Component | Spec ch. | Msg range |
|---|---|---|---|
| NSFMM | Memory Manager | 2 | 100–199 |
| NSFBUF | Buffer Manager | 3 | 100–199 |
| NSFQUE | Queue Library | 4 | — |
| NSFEVT | Event Dispatcher | 5 | 000–099 |
| NSFTMR | Timer Manager | 6 | — |
| NSFTRC | Trace Facility | 7 | — |
| NSFSTS | Statistics | 8 | — |
| NSFDEV / NSFCTCI / NSFLCS / NSFHOST | Devices & drivers — NSFDEV table + DEVOPS/DEVIO seams (`src/nsfdev.c`); CTCI top half `asm/nsfctcio.asm`, SVC 99 + lifecycle `src/nsfctci.c`, codec `src/nsfctcif.c`, bottom half + I/O subtasks `src/nsfctcib.c` over `nsfthr` (ADR-0022/0023); host driver `src/nsfhost.c` | 9 | 200–299 |
| NSFSOC / NSFREQ | Sockets (`src/nsfsoc.c`: SOCKCB, `(gen<<16)\|id` descriptor, `PROTOPS`, parked requests, `soc_destroy`) / request manager (`src/nsfreq.c`: queue + `requestECB`, dispatcher, app registry); `NSFRQE` in `include/nsfreq.h`, **FROZEN** | 10 | 600–699 |
| NSFIP / NSFICM | IPv4 / ICMP (`src/nsfip.c`, `src/nsficmp.c`) + RFC 1071 checksum over a PBUF chain (`src/nsfcksum.c`); ADR-0024, byte-wise big-endian | 11 | 300–399 |
| NSFUDP | UDP (`src/nsfudp.c`, ADR-0028: checksum seed, IP demux registration) | 12 | 400–499 |
| NSFTCP | TCP (`src/nsftcp.c`, ADR-0031–0033) / SELECT engine NSFSEL (`src/nsfsel.c`, ADR-0035) | 13 | 500–599 |
| NSFCFG | Configuration | 14 | 700–799 |
| NSFEZA | EZASOKET API | 15 | 600–699 |
| NSFOPR | Operator interface (dispatcher + CIB seam; M0-8) | 5 / 17 | 800–899 |
| NSFMSG | WTO message seam (libc370 `wto`; M0-8) | 5 / 17 | — |
| NSFSTC | STC startup + NSFCFG→init wiring (M0-8) | 5 / 14 | 000–099 |
| NSFFMT | Safe formatting seam (`nsf_vsnprintf`/`nsf_snprintf`; libc370 truncation fix, ADR-0026, issue #25) | — | — |
| NSFS / NSFSX / NSFREQX / NSFV | Phase 2: NSFS STC (`src/nsfsmain.c`, `src/nsfsx.c`), crossing + classifier (`src/nsfreqx.c`), client seam (`src/nsfreqc.c`), SVC routine `asm/nsfvsvc.asm`, probe STC NSFV (`src/nsfv.c`); ADR-0038–0048 | — | 000–099 |
| (recovery) | ESTAE via libc370 `__estae` + C `nsf_recover` (ADR-0018; no NSFESTAE CSECT) | 17 | 900–999 |

## SMP4 FMID

Proposed: **`TNSF010`** for 0.1.0, the first level (no `delete`). The FMID
rules, the burned ids and the test-install trap are in the root `CLAUDE.md` and
`~/repos/mvs/.workspace/context/smp.md`.
