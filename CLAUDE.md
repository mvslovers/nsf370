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
| `docs/Project-Brief-v2.md` | Why / scope / constraints / milestones | **Frozen** — do not expand |
| `docs/Architecture-Specification.md` | How: interfaces, data structures, lifetimes | Living, versioned |
| `docs/adr/ADR-*.md` | Rationale for individual decisions | Append-only |
| `CLAUDE.md` (this file) | Operating rules + status | Update on convention changes |
| Notion → *Issues & Tasks*, project **mvs38j-ip (NSF)** | Task tracking (`[NSF] Mx-y`) | Mirrors §7 below |

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
  calls the same destroy/quiesce functions as the orderly path. A crash must
  never require a Hercules restart to clean up.

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

**C-callable HLASM (entry convention)**
- Every C-callable HLASM routine is built the **standard cc370 way** — `COPY
  MVSMACS` + `COPY PDPTOP`, `FUNHEAD` prologue, `FUNEXIT` epilogue (`FUNEXIT
  RC=(Rn)` when it returns a value in R15) — modeled on libc370
  `asm/@@getclk.asm`. **Never hand-roll `STM`/`BALR`/`USING`:** a hand-rolled
  seam omits the `ENTRY` / name eyecatcher / `LR R12,R15` base the cc370
  C-runtime path (`@@CRTGET`) relies on, and ABENDs the *next* C library call on
  MVS (S0C6) while it links and host-tests perfectly clean — a general
  mainline-runtime blocker (issue #8, proven by staged isolation).
- A **leaf** routine (issues no SVC/macro, calls nothing) uses the plain
  `FUNHEAD ,` form (like `nsf_now`). A routine that **issues an OS macro/SVC**
  (OPEN/CLOSE/EXCP/WTO/…) must give its callee a save area: use `FUNHEAD
  SAVE=name,US=NO` with a static `name DC 18F'0'` in the CSECT (single-task, so
  one shared static area is safe, per `nsfctcio.asm`) — **not** the leaf form.
- The `FUNHEAD` entry name IS the 8-char `asm()` alias, character for character.
  Address static data by **explicit displacement** `LABEL-entry(,R12)` (or a
  register), never a bare-label `USING` (as370 drops those to base 0 — the S102
  class). Address a **DSECT field** (IEZIOB/IHADCB) as a difference expression
  `FIELD-<origin>(Rn)`, never a bare `FIELD(Rn)`: as370 assembles a bare
  `FIELD(Rn)` with `disp = FIELD − (active USING base address)` instead of `disp
  = FIELD` — wrong for any USING base off the CSECT origin, e.g. every `FUNHEAD`
  entry after the first (a runtime-only wrong field, MVS-only; cc370 #18). Keep
  `CS`/`LM` (RS-format)
  operands `D(B)`, never `D(,B)` (#5). Keep every statement inside **column 71**:
  as370 reads column 72 as a continuation flag, so a **comment that overruns
  column 71 on an INSTRUCTION line** makes as370 treat the next line as a
  continuation and **silently drop the operand — or the whole instruction**. The
  M3-0b S0C1 was exactly this: an over-long comment on the `SVC 33` line dropped
  the `SVC 33` itself (and the save-area restore), so `ctci_halt_read` returned
  through garbage and branched to low storage. **A green host build and a clean
  cc370/as370/ld370 link are NOT evidence** — the merge is invisible off-target;
  only the live ABEND (or the `as370 -a=` listing) shows it. Keep
  instruction-line comments short and within column 71.
  **CORRECTED 2026-08-28: a WHOLE-LINE `*` comment is NOT exempt, and the old
  advice to "put long rationale in a leading `*` comment block (those are
  full-width and safe)" was wrong — it is what produced the damage.** IFOX00
  applies the continuation rule to comment statements too (`PNXT13` reads them
  with `RALLCNT`), so a `*` card reaching column 72 **eats the next card**;
  as370 short-circuited comments before looking at column 72 and so disagreed
  with the target until mvslovers/cc370#72 (`IFO026`/`IFO069`, severity 4).
  Measured here on the fixed as370: `asm/nsfctcio.asm` line 95, a 74-byte
  comment reading "DSECTs before the code", swallowed the `DCBD DSORG=PS` on
  the next line, so **every** DCB DSECT symbol resolved to 0 —
  `MVC DCBDDNAM-IHADCB(8,R2),0(R3)` assembled as `MVC 0(8,R2),0(R3)` and
  `TM DCBOFLGS-IHADCB(R2),DCBOFOPN` as `TM 0(R2),X'00'`, a mask that can never
  be true. In `asm/nsfvsvc.asm` it dropped `L R3,REQFUNC(,R8)` outright — the
  SVC routine's dispatch value — a 646-byte object difference. **The rule is
  therefore: NO card of any kind, comment or instruction, may reach column 72.**
  Since mbt#94 a warned assembly (RC 4) no longer fails the build, so a
  recurrence is silent again: the `as370 -a=` listing and an old-vs-new object
  comparison are the gate, not a green build.
- **`as370` knows five instruction formats and three S-format instructions.** Its
  table is RR/RX/RS/SI/SS plus exactly `IPK`/`SPKA`/`STCK` — there is **no SSE and
  no RRE format at all**. Anything outside that set must be emitted as raw bytes
  (`DC X'…'`) with the encoding derived from **primary source** (the Hercules
  `opcode.c`/`control.c` for this target, not memory), and there are **two distinct
  failure modes with one rule**: a mnemonic as370 *knows* can assemble to silently
  **wrong** bytes — `MVCK` is in the table as plain `F_SS`, so it drops the `R1`
  length and `R3` key registers (hence raw `D9`, ≤255-byte pieces, ADR-0039) —
  while a mnemonic it does **not** know cannot assemble at all: `SSK`/`ISK` (raw
  RR `08`/`09`) and `MVCDK`/`MVCSK` (SSE `E50F`/`E50E`, and **absent on this
  target** — they take `S0C1`, ADR-0039). `LRA` (RX `B1`) *is* in the table.
  **The `as370 -a=` listing is the gate on the emitted bytes — never a green link.**
- **`SSK` sets the key on a REAL frame, and a pageable page moves underneath it.**
  `SSK`/`ISK` take a *real* address, so manufacturing a protected page means
  `LRA` the virtual page → `SSK` the real frame → touch the virtual page. If MVS
  steals and rebinds that page in between, **the key was set on a frame the
  program no longer has** — measured as three different real frames for the same
  virtual page across three runs. Used in `test/mvs/tstmvcd.c` 2.2b and
  `test/mvs/tstmvck.c` scenario 3. **The direction of the error is what decides
  whether it invalidates a conclusion, and it is one-directional:** both tests
  expect the access to **fault**, so a lost key means no fault means the
  assertion **FAILS** — verified against *every* `SSK`-dependent assertion in
  both files (2.2b's `ISK` read-back reads the *saved real address*, so it stays
  a true statement about that frame and cannot mask the store check;
  `tstmvck.c`'s `keyback` is logged, never asserted). So the technique yields
  **false negatives, never false positives**: a run in which the assertion
  PASSED is trustworthy, and ADR-0039's Stage-0b conclusion stands unchanged —
  do not reopen it on the strength of "the technique is flaky". Prefer a
  destination needing no `SSK` at all (a `GETMAIN SP=241` block is key 0 by
  allocation — `tstmvcd.c` 2.2a, which this hazard cannot touch) and keep the
  `SSK` variant only as a second, independent reading.
- **Exception:** a routine the OS invokes as an *exit* (not called from C) is not
  a C callee and does not get `FUNHEAD` — e.g. `NSFTMEXP`, the STIMER exit.
- **Reviewer checklist (assembler):** a new C-callable routine uses
  `FUNHEAD`/`FUNEXIT`, not hand-rolled; its entry name matches its `asm()` alias;
  data addressed by explicit displacement; `CS`/`LM` stay `D(B)`; nothing past
  column 71. A green host build and a clean link are **not** evidence — this
  failure is MVS-runtime only (issue #8).

**Standard library truncation (libc370, ADR-0026)**
- `vsnprintf`/`snprintf` on libc370 do **NOT** NUL-terminate on truncation — a
  real glibc/C99 violation, pinned live by `TSTVSNP` (issue #25): `size` IS a
  hard write bound (not a memory-safety bug), but the target is filled solid
  through byte `size-1` with data, leaving no byte for a terminator; the
  return value is still the C99 "would-be length" regardless. **Never call
  `vsnprintf`/`snprintf` directly** — use `nsf_vsnprintf`/`nsf_snprintf`
  (`include/nsffmt.h`), which always NUL-terminate when `size > 0` and return
  the count of characters actually in the buffer (clamped), not the raw C99
  value.

**Contracts**
- `NSFRQE` (the app↔stack request block) **freezes at the M3 exit gate**.
  Changing it afterwards requires an ADR.

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
- **C compiler:** `cc370` — complete host cross-compile suite. **Not**
  `c2asm370`.
- **C runtime:** `libc370` — the `cc370` **sysroot** (target C library),
  provided by the toolchain; **not** a `[dependencies]` entry (ADR-0014).
  **Not** `CRENT370`.
- **Assembler / linker:** IFOX00 / IEWL on the target (HLASM modules).
- **Build orchestration:** **MBT V2** (MVS Build Tools). Repo shape is an MBT
  project: `project.toml` at root; local MVS connection in an un-committed
  `.env` (from `.env.example`).

**Two build worlds, both driven by MBT (ADR-0014):**

| World | Builds | Compiler | Needs MVS? | Runs in CI? |
|---|---|---|---|---|
| Host (`make test-host`) | portable C + `nsf*_host.c` shims | native cc | No | **Yes** |
| MVS (`make test-mvs`, `deploy`, …) | full stack incl. `asm/*.asm` | cc370 + IFOX00 | Yes (live 3.8j over IP) | No |

Both are MBT V2 targets — there is **no** separate `host.mk`. The host build
is configured by the `[host]` table in `project.toml`; a `[host].replace`
map swaps each MVS-only CSECT (`asm/*.asm`) for its `src/*_host.c` shim.
Rules: `asm/*.asm` never compiles on host; everything else compiles both
ways; warnings-as-errors everywhere. The distinction between the worlds is
MVS reachability, not MBT vs. not-MBT.

**Commands (MBT V2 — real target names; `make help` for the full list):**
```
cp .env.example .env      # once: fill in MVS connection details
make test-host            # native build + run of the portable tests (no MVS)
make deps                 # resolve deps + allocate target datasets on MVS
make test-mvs             # deploy + run the tests on MVS
make modules              # cross-compile + assemble the load modules on MVS
make package              # TRANSMIT/XMIT the load library for download
make deploy               # upload modules + RECV370 on MVS
```
There is no `bootstrap`/`build`/`link` target (`deps` is the former
`bootstrap`). At M0-1 the project is module-less (test-only), so `modules`/
`package`/`deploy` become meaningful from M0-2/M1 onward.

**C dialect:** `-std=gnu99` (as set in `project.toml`), used conservatively —
no VLAs, no runtime allocation, fixed-width via the project typedefs
(`UCHAR/USHORT/UINT/INT`), big-endian S/370, AMODE 24 / RMODE 24, EBCDIC on
target. `cc370` accepted these flags on both host and MVS at M0-1; record any
later `cc370` limit surprises in an ADR. Comments and documentation in
**English**.

### Live MVS target & autonomous operation (`mvsdev`)

Operational runbook for driving the live target without a human in the loop.
The target is **MVSCE on Hercules on the host `mvsdev`** (`mvsdev.lan`).
Credentials live in the un-committed `.env` and the zowe `mvsdev` profile —
never hardcode them.

- **Build / deploy / test go over mvsMF REST (no SSH):** `make deps` (allocate
  datasets), `make deploy` (→ `NSF.LINKLIB`), `make modules`, `make test-mvs
  ARGS="--only TSTX"`. `test-mvs` writes the **full job spool to
  `build/test-runner.spool`** — read it for the actual `printf`/message output,
  not just the PASS/FAIL matrix. MBT honours **only `[build].cflags`** (no
  per-test cflags — `mbtconfig.py`), so a `-D`-gated device test is compiled in
  by adding the define to `[build].cflags` **temporarily** (revert before commit).
- **Console commands = ZOWE + the console log.** Issue any command with
  `zowe zos-console issue command "<cmd>"` (default profile `mvsdev`): `S NSF`,
  `F NSF,DISPLAY`, `P NSF`, `D U`, … **Direct output is unreliable** (often only
  the first message line returns), so read the full result from the
  Hercules-captured console log: `ssh mvsdev "grep -A6 '<marker>'
  ~/MVSCE/mvslog.txt"` (`mvslog.txt` is the complete MVS console/syslog).
  For a time-bounded view of recent messages use `zowe zos-logs list logs
  --range 15m` (last 15 minutes; also how you wait out the syslog LAG — see M1
  live-run rules). Grep the RIGHT reply IDs: STATS answers `NSF810/811I`, only
  DISPLAY answers `NSF800-802I`. When gauging egress on a CTCI ping, `LNK1 out`
  is the device-transmit counter; since the issue-#21 fix (ADR-0025 pair
  sequencing) it tracks `NSFIP out`/`NSFICM outecho` exactly — a lasting lag
  now indicates a real regression ([[nsf370-ctci-write-tail-stall]]).
- **Live-run friction that each cost real time (issue #40 gates).** (1) `make
  test-mvs` can report **"NO RC" / FAIL while the job actually finishes CC 0** —
  the mbt job-poll times out on a long CTCI device job (e.g. TSTTCPD's guest waits
  out extra ~90 s accept windows). Don't trust the matrix: poll the jobid to
  `OUTPUT` (`zowe zos-jobs view job-status-by-jobid JOBnnnnn`) and read its spool
  (`zowe zos-jobs view all-spool-content JOBnnnnn`). (2) `zowe zos-console` **500s
  intermittently on `P NSF`/`S NSF`** ("Internal server error (abend recovery)")
  but the command **still lands** — confirm from `mvslog.txt` (`NSF830I`/`NSF000I`),
  don't re-issue blindly. (3) **Back-to-back heavy CTCI runs degrade the pair
  (MIH, IGF991I/995I)** — rapid-fire UDP echo throughput suffers on a degraded
  pair while ping and *paced* echo stay clean; the M3-5 rapid 1000/1000 echo needs
  a fresh/idle pair, not a code fix.
- **`make deploy` fails while an STC holds `NSF.LINKLIB` — and the run afterwards
  silently tests the OLD binary.** Order is **`P NSFS` (or `P NSFV`) → `make deploy`
  → `S NSFS`**. **Signature:** a *mid-chain* `HTTP 500 Internal Server Error for
  DELETE /restfiles/ds/NSF.LINKLIB: {"rc":8,...,"message":"Dataset delete failed"}`
  — easy to miss when `deploy` is chained after `make modules`, because the lines
  that follow it still look like a normal deploy. **The tell, which is the part
  worth remembering:** *identical values across supposedly different builds.* Every
  "live" result after a failed deploy is about the previous module and nothing
  complains — this is the project's most expensive failure class in pure form.
  It cost a full diagnostic cycle in M5-2b2, where three runs of "different" routers
  returned byte-identical numbers. **Re-run rather than reason about it.**
- **`ssh mvsdev`** (host-side work: `tun0` captures, reading Hercules source,
  `/proc`) is hardened **keychain-independent** — on-disk `~/.ssh/id_rsa`,
  `IdentityAgent none`, `accept-new`, via the `mvsdev` block in `~/.ssh/config`
  (user `mike`). A **reinstall changes the host key** → self-heal with
  `ssh-keygen -R mvsdev` then reconnect. `ping` and `tcpdump` (cap_net_raw) run
  **without sudo**; **sudo needs a password we do not have**, so anything needing
  root must be asked of the user.
- **Hercules / CTCI on this box.** Config `~/MVSCE/conf/local/custom.cnf`; the
  **exact running Hercules source** is at `~/hercules/hyperion/` (read
  `ctc_ctci.c` etc. directly — primary source beats docs). The CTCI pair is
  `0500,0501 CTCI 192.168.200.1 192.168.200.2` on `tun0` (guest `.1` / host
  `.2`), CUU 500/501 online, 502/503 offline. To drive a CTCI device test, build
  with `-DNSFCTCI_CUU=0x0500 -DNSFCTCI_SRC=0xC0A8C801u -DNSFCTCI_DST=0xC0A8C802u`;
  the guest READ blocks until inbound traffic, so background a continuous
  `ssh mvsdev "ping -i 0.5 192.168.200.1"` to trigger it and `tcpdump -ni tun0
  icmp` to see the WRITE (`/proc/net/snmp` `Icmp InEchos` is a no-sudo
  WRITE-egress proxy).

---

## 6. Repository Layout (spec §16.2)

Flat layout (ADR-0014); the `nsf*` prefix namespaces every source, so no
per-layer subdirectories. Components stay grouped by prefix and the §9 map.

```
<repo>/
├── project.toml      MBT V2: modules, sources, tests, deps, [host] table
├── Makefile          two lines: MBT_ROOT := mbt + include mbt/mk/mbt.mk
├── .env.example      MVS connection template (.env is git-ignored)
├── CLAUDE.md         this file (repo root — NOT under docs/)
├── mbt/              MBT V2 — a git submodule
├── src/              portable C: nsfmm.c nsfbuf.c nsfque.c nsftmr.c
│                     nsfevt.c nsftrc.c nsfsts.c nsfip.c nsficmp.c
│                     nsfudp.c nsftcp.c nsfsoc.c nsfreq.c nsfeza.c
│                     nsfdev.c nsfctci.c … + nsf*_host.c host shims
├── asm/              HLASM: nsfctcio.asm nsfxq.asm nsfstim.asm nsftime.asm
│                                          ← ONLY MVS-specific asm.
│                     (WTO / CIB / ESTAE reuse libc370 seams, not hand-rolled
│                      nsfwto.asm/nsfestae.asm — ADR-0018; the async STIMER
│                      exit lives in nsfstim.asm. MVS-only C glue that is NOT
│                      asm — nsfmain.c, nsf*_plat.c — sits in src/.)
├── include/          one header per component (nsf.h + nsf*.h)
├── cfg/              sample PROFILE.TCPIP members
├── jcl/              install/SAMPLIB jobs (driven by MBT)
├── test/             dual host+MVS tests (tstsmoke.c, …)
│   ├── mvs/          Level 2/3: on-MVS component & integration jobs
│   └── asm/          HLASM test callers
└── docs/
    ├── Project-Brief-v2.md
    ├── Architecture-Specification.md
    └── adr/          ADR-0001 …
```

---

## 7. Milestone Status

Mirrors Notion (*Issues & Tasks*, project *mvs38j-ip (NSF)*). Update the Status
column as milestones progress. Every milestone's **Definition of Done** =
host Level 0/1 green in CI + the demonstrable deliverable shown on Hercules via
MBT `build`/`link` + **leak gate** (all pools back to baseline in-use after
quiesce) + spec/ADRs updated.

| MS | Scope | Exit gate | Status |
|----|-------|-----------|--------|
| **M0** | Foundation: MM, buffers, queues, timers, event loop, trace, stats, config, STC skeleton + ESTAE | `F NSF,DISPLAY,STATS` answers; clean stop, pools at baseline; CI green | ✅ **Complete** (host + cross-link + **on-MVS operator run VALIDATED live** on TK5) — M0-1 done (skeleton); M0-2 done (NSFQUE + NSFMM + nsf_abend); M0-3 done (NSFBUF: PBUF, headroom, chains, two-pool leak gate); M0-4 done (NSFTRC ring + NSFSTS registry + shared `nsftime` seam `nsf_now`/`nsf_taskid`, ADR-0016; TSTTRC 23/23, TSTSTS 23/23 host-green); M0-5 **done** (NSFTMR sorted delta queue + `nsfstim` STIMER seam; TSTTMR 43/43 host-green, 226/226 suite; ADR-0011 corrected **STIMERM→STIMER**; S102 seam bug fixed). **Issue #8 FIXED:** the hand-rolled C-callable HLASM seams (`nsftime`/`nsfxq`/`nsfstim`) are rebuilt on the standard cc370 entry convention (COPY MVSMACS + PDPTOP, FUNHEAD/FUNEXIT, per `@@getclk.asm`) — hand-rolled `STM/BALR/USING` was breaking the cc370 C-runtime (`@@CRTGET`, S0C6). Stage-2 isolation now **CC 0** (nsf_now + nsf_taskid, PSATOLD proven); `nsftime` VALIDATED. **ADR-0011 gate MET and FROZEN:** accuracy job on MVS = mean 100.1/100.2 ms, min/max 100 ms, jitter 0 ms (both criteria pass). `nsfxq`/`nsfstim` keep deferred-runtime status for M0-6 (entry convention fixed; xq handoff + async STIMER exit validated at M0-6). **M0-6 done** (NSFEVT event dispatcher / main loop per §5.3: WAIT-unless-pending → NSFXQ handoff drain → dispatch under a 64 drain budget → `nsftmr_run` → shutdown; the WAIT/POST seam is libc370 `ecb_waitlist` on MVS / a pthread cond-var host shim. **Timer wakeup = ADR-0017: the async STIMER REAL exit** — `NSFTMEXP` corrected to the documented MVS 3.8 exit linkage (GC28-0683) and **RUNTIME-VALIDATED**: TSTEVT 17/17 host-green (dispatch order, drain budget, pthread-simulated xq handoff, shutdown leak gate; 243/243 suite); TSTEVTM on MVS **CC 0**, 10 heartbeats at mean 100.2 ms, clean shutdown — the S0C6 is gone). **M0-7 done** (NSFCFG PROFILE.TCPIP parser + immutable fixed-size `NSFCFG` output struct per §14: `cfg_parse` (pure C over a buffer) + `cfg_load` (fopen/fread wrapper); DEVICE/LINK/HOME/GATEWAY/PORT/TCPCONFIG/UDPCONFIG + the `NSFPOOL`/`NSFTRACE` extensions; all-or-nothing validation rejecting on any error with an `NSF7xxE` message + 1-based line number, no partial config (eyecatcher stamped only on success), warn+continue for an explicit ignorable list. **Charset-transparent** (spec 15.3): compares char/string literals only + block-wise EBCDIC/ASCII-safe case fold — no hardcoded byte values — so the same source parses the ASCII host corpus and an EBCDIC PDS member. Referential integrity (LINK→DEVICE, HOME→LINK) deliberately **deferred to M0-8** (spec 14.2 is silent on the ordering/reference rules it would require). TSTCFG 111/111 host-green over a 14-file `test/cfg/` corpus (4 valid + 10 broken, one error class each, exact line asserted); **354/354 suite**, `-Wall -Wextra -Werror` clean; cross build: cc370 → 2 unique externals (`NSFCFPRS`/`NSFCFLDR`) → as370 assembles clean. **M0-8 done** (MVS STC skeleton — the `NSF` load module: config-driven init → §5.3 loop → `F NSF,DISPLAY/STATS/TRACE comp ON|OFF/STOP` + `P NSF` → orderly shutdown, under ESTAE. Operator = a **portable dispatcher** `nsfopr_dispatch` over a thin CIB/QEDIT seam; the loop's §5.3 **cibECB slot** is filled by `evt_set_operator`, whose drain runs **unconditionally** each pass — the `IEE342I TASK BUSY` startup-CIB trap from ufsd. **NSFCFG→init wiring** `nsf_init_from_cfg`: NSFTRACE→`nsftrc_flags`, NSFPOOL→`buf_init_counts`, and the M0-7-deferred **referential integrity** (LINK→DEVICE, HOME/GATEWAY→LINK; `NSF720/721/722E`). Recovery + WTO reuse **libc370** (`__estae` + C `nsf_recover` percolating via `SDWARCDE=SDWACWT`; `wto`) — **ADR-0018**, maintainer-ratified; **no** `nsfestae.asm`/`nsfwto.asm` (a raw asm→C recovery bridge re-implements `@@estae` = issue-#8 class); M0-8 adds **zero** new asm. Host **354→408** (TSTOPR 25, TSTSTC 29); NSF module + all 15 test modules cross-link clean (cc370/as370/ld370), alias scan clean (statics `ENTRY=NO`, all exports unique). **VALIDATED LIVE on TK5** (mvsdev, STC00175): deployed `NSF.LINKLIB` + `SYS2.PARMLIB(NSFPRM0)` + `SYS2.PROCLIB(NSF)`; JESMSGLG shows NSF000I→NSF001I startup, `F NSF,DISPLAY/STATS/TRACE IP ON/HELP` all replied (DISPLAY showed the deployed config; **TRACE FLAGS 0200→0201** proved the IP toggle took effect on EBCDIC), `P NSF`→NSF830I→NSF011I→`IEF142I ... COND CODE 0000`, SYSUDUMP DD empty (no dump, verified from the full spool). `test/mvs/tststcm.c` covers ESTAE establish/delete; the induced-ABEND→percolate path was not force-run (leaves a dump). **M0 COMPLETE. M1 (CTCI driver + NSFDEV + NSFHOST) next.**) |
| **M1** | CTCI driver (HLASM top / C bottom) + NSFDEV + NSFHOST | ping → hexdump in trace; crafted packet seen in host `tcpdump` | ✅ **Complete** — **M1-1 done** (CTCI wire format verified byte-exact vs Hercules `ctc_ctci.c`, written into spec §9.3 as normative: 3088 read/write pair, `CTCIHDR`/`CTCISEG`, big-endian). **M1-2 done** (device abstraction + host driver): `include/nsfdev.h`/`src/nsfdev.c` — the `DEVOPS` contract + fixed device table (`dev_register`/`dev_find`/`dev_find_cuu`/`dev_by_index`/`dev_foreach`/`dev_start`/`dev_send`/`dev_shutdown`), `NETDEV` 64 B (`NSF_SIZE_ASSERT`), `send`-ownership rule enforced. The executive loop stays **driver-agnostic** (never names HOST/CTCI): `NSFDEV` registers three hooks via `evt_set_devices` (mirroring `evt_set_operator`) — `nsfdev_collect_ecbs` (device ECBs→ECBLIST), `nsfdev_poll_input` (drain each `doneq`→`EV_PACKET_RECEIVED`, before dispatch, lost-wakeup-safe ECB clear, drop+count on EVT exhaustion), `nsfdev_kick_output` (§5.3 step 5); `nsfevt_wake` kicks output for a send from outside a loop pass. **NSFHOST** (`src/nsfhost.c`, host-only): `DEVOPS` impl whose inbound path is the **host analog of the CTCI I/O-completion exit** — a pthread reader thread `xq_push`es a received PBUF onto `doneq` + POSTs the device ECB, so the `doneq→EV_PACKET_RECEIVED` handoff is validated across a real thread boundary (M1-3 swaps only the producer). Default in-memory **loopback** (copy-free relay; NSFMM touched only on the executive task → no pool race), optional **TUN** (`-DNSFHOST_TUN`), **PCAP** reserved. Cross-build discipline: `nsfhost.c` is host-only, the MVS build compiles the NULL-ops placeholder `src/nsfhost_plat.c` (no host driver on MVS — use CTCI/LCS) swapped via `[host].replace`, so `test/tstdev.c` (portable; pthread lives in nsfhost.c) still cross-links and skips trivially where `nsfhost_ops()` is NULL. Host **408→488** (TSTDEV 80: send→receive cycle, in-order delivery, bounded `sendq`, DOWN-device reject, leak gate; 80/80 stress-stable); `-Wall -Wextra -Werror` clean (host + cc370); NSF module + all 16 test modules cross-link clean, alias scan clean (unique `NSFD*`/`NSFH*`/`NSFEVDEV`/`NSFEVWK`). ADR-0018/spec §9.2/§9.4/§19 updated. **M1-3 done** (CTCI top half + C lifecycle, per **ADR-0019** — plain `EXCP`, IOS posts the IOB ECB, **no** I/O-completion exit and **no** appendage): `asm/nsfctcio.asm` (six C-callable FUNHEAD entries — `ctci_scb_size`/`ctci_open_sub`/`ctci_read`/`ctci_write`/`ctci_status`/`ctci_close_sub`; the OPEN/CLOSE/EXCP-issuing ones use `FUNHEAD SAVE=`, not the leaf form; DCB/IOB fields addressed as `FIELD-<origin>(Rn)` difference expressions to dodge an as370 bug (cc370 #18: a bare `FIELD(Rn)` gets `disp = FIELD − active-USING-base`); file named `nsfctcio` because mbt derives the object name from the source basename and `nsfctci.asm` would collide with `src/nsfctci.c`). `src/nsfctci.c` — reserve NSFMM init-window pools (CTCIDEV/CTCISCB/CTCIBUF), SVC 99 allocate the CUU pair via the libc370 seam (`DALUNIT` from the DEVICE-statement CUU, system-returned DDNAME `DALRTDDN`, `DALSTATS` SHR), open both subchannels, start/decode/close raw EXCP; `NSF2xxE` on failure (refuse-to-start), unallocate on close. `include/nsfctci.h` (`CTCIDEV` 64 B `NSF_SIZE_ASSERT`, `CTCIHDR`/`CTCISEG` wire structs, 14 unique `NSFCI*` aliases). `test/mvs/tstctcm.c` (host=false). **PROVEN:** host 488/488 unchanged; cc370/as370/ld370 cross-link clean (NSF module + all 17 test modules); alias scan clean; and — **run live on TK5** (`test-mvs` TSTCTCM CC 0 batch+TSO, 6/6) — the **SVC 99 seam end-to-end** over our `svc99_call` wrapper: FAILURE (`ZZZZ` → `rc=4 S99ERROR=021C`, `__svc99` standard-linkage confirmed, no S0C6) **and SUCCESS** (a device-free DUMMY allocation → `S99VRBAL rc 0`, generated DDNAME `SYS00007` reaches our buffer, `S99VRBUN rc 0`, no stray DD); plus the **full `ctci_dev_open` lifecycle** (reserve the 3 NSFMM pools on 3.8j, `mm_alloc`, `%04X`, SVC 99 of the numeric undefined CUU `0E20`, the `NSF202E` WTO on the console, refuse-to-start → NULL, cleanup) — everything ctci_dev_open does except the channel I/O. **EXCP path VALIDATED live on MVSCE** (issue #16, real Hercules CTCI pair CUU 500/501 on `tun0`): `test-mvs` TSTCTCM **CC 0, 12/12** — SVC 99 allocated devices 500/501 (two distinct DDNAMEs), OPEN, EXCP **WRITE** post `X'7F'` (crafted ICMP echo seen in host `tcpdump`, id `0xABCD`), EXCP **READ** post `X'7F'` (length = requested − IOB residual; block walked to 227 well-formed `CTCISEG`s). **Two fixes fell out (ADR-0020):** the SVC 99 unit name is **3 hex digits not 4** (`%04X`→`%03X`; a 4-digit CUU is undefined = `S99ERROR 021C`, which had made the old `0E20` wall-probe a false positive — see [[nsf370-device-number-width]]), and **§9.3's READ framing was wrong** (one block of `CTCISEG`s, leading `hwOffset` = end-of-data, Hercules does NOT transfer the `0x0000` terminator to the guest, `hwType` a constant `0x0800` marker; the WRITE framing was correct). Deferred-seam labels removed from `asm/nsfctcio.asm`+`src/nsfctci.c`; the `CTCISAVE`/ESTAE item-3 constraint is now a source comment. **M1-4 done (incl. M1-4b, issue #18 / ADR-0022+0023) — M1 exit gate MET, validated live in the STC.** The C bottom half: the **codec** `src/nsfctcif.c` (CTCISEG↔raw-IP, byte-wise big-endian; TSTCTCIF 37/37 on literal §9.3 vectors), the **portable bottom half** `src/nsfctcib.c` (DEVOPS + the repurposed DEVIO seam + the two **I/O subtask functions** read_sub/write_sub; TSTCTCI 44/44 over the host thread+channel shims, run as real pthreads), the **channel/SVC 99 split** (`ctci_chan_alloc/unalloc`, executive-side) and the new **`nsfthr` threading seam** (libc370 `cthread` on MVS / pthread on host — the SAME subtask logic runs both ways; de-risked in ISOLATION first: **TSTCTHR** CC 0 on MVS — a subtask SVC-2 POST into the executive's multi-ECB WAIT wakes it 20/20 alongside the STIMER heartbeat, termecb join + detach, ESTAE-isolated subtask fault; prerequisite: unauthorized NSF must call `clib_identify_cthread()` itself, since libc370 only IDENTIFYs CTHREAD inside the authorized `clib_apf_setup`). **Completion model (ADR-0022/0023):** the executive WAITs ONLY on `dev->ecb` and CLEARS it before each service (a lingering posted ECB in the multi-ECB WAIT is the #18 hazard); each subchannel is OWNED by its subtask (OPEN+EXCP+CLOSE on one TCB, so its CLOSE purges its own EXCP — this forced a **per-scb save area** in `asm/nsfctcio.asm`: the shared static `CTCISAVE` was corrupted by concurrent subtask calls → live S238); READ is **single-block-synchronous** (ONE buffer + a `returnecb` handshake; the executive decodes raw blocks into PBUFs — the doneq payload is raw blocks, NOT subtask-allocated PBUFs, because NSFMM is deliberately unserialised, §3; ping-pong is the documented throughput follow-on); WRITE is one-outstanding (executive encodes into wbuf, subtask EXCPs, executive reaps + frees the PBUF exactly once). `nsfthr` waits use `ecb_(timed_)waitlist` with a SEPARATE timeout ECB — `cthread_wait` CLEARS the ECB and `ecb_timed_wait` POSTs it on timeout, either losing or forging a completion; a join timeout RETAINS (never detaches) a live subtask. **Idle liveness = the ADR-0017 heartbeat armed at STC start** (`nsftmr_plat_arm(1)` in nsfmain.c): a timed executive WAIT is disqualified because `ecb_timed_waitlist` TTIMER-CANCELs the calling task's interval timer (**STIMER is a per-task singleton**) and its timeout does not fire on the CRT main task (ADR-0023 §6; consequence: `nsfthr_timed_wait/join` run on the executive only outside the heartbeat window — start before arm, teardown after disarm). `CTCIDEV` 68→108 (`NSF_SIZE_ASSERT`); host suite **569** (TSTCTCI rewritten for the subtask model); cross-link + alias scan clean (new `NSFTH*`, `NSFCIALC`/`NSFCIUNA`). **Validated live on MVSCE** (STC on the real pair 0500/0501): `S NSF` → SVC 99 + both subtasks OPEN (`NSF210I/211I`); host ping → reads decoded on the executive (`ctr_in` 0→59→86) + **RX hexdump in the trace ring**; **`F NSF,STATS` prompt in EVERY state** — fresh idle, during reads (the exact operation #18 hung), and post-traffic idle; **MIH across idle tolerated** (IGF991I/995I, device kept working after the I/O restart); **`P NSF` → NSF830I→NSF011I→IEF404I within one second**, subtasks joined (termecb), **SYSUDUMP empty (0 bytes)**; the crafted ICMP echo id `0xABCD` on the host wire in `tcpdump` (TSTCTCM, whose part 2 also proved the isolated subtask path batch CC 0, 11/11, incl. a MULTI-ECB-WAIT mini-loop — never again a single-ECB probe, which was the #18 blind spot). Live-run rules that each cost real time: MVSCE syslog LAGS — wait before reading (`zowe zos-logs list logs --range Nm`); grep the RIGHT reply IDs (STATS answers `NSF810/811I`, only DISPLAY answers `NSF800-802I`); background pings on mvsdev need `setsid` + on-wire verification (`tcpdump`), else a zombie ping fakes an idle-hang; TSTCTCM part 2 is a SINGLE-SHOT batch probe (the TSO re-run against the same physical pair back-to-back stalls MIH-pending — a live-hardware re-use artifact, not a driver bug). **M1 COMPLETE** (gate: ping → RX hexdump in trace; crafted packet in host `tcpdump`). LCS + ARP remain M6. **M2 (IPv4/ICMP) next.** |
| **M2** | IPv4 in/out + routing + ICMP echo/errors + checksum | `ping <mvs-ip>` sustained, 0 loss on loopback link | ✅ **Complete.** M2-1: `in_cksum` (RFC 1071) over a **PBUF chain** — word parity relative to `off` so a word straddling an ODD segment boundary is summed right (`src/nsfcksum.c`, alias `NSFCKSUM`; TSTCKSUM 10/10 on literal RFC/IP-header vectors, pinned before any packet code, spec 11.5). M2-2: **NSFIP** (`src/nsfip.c`) `nsfip_input` (validate v4/IHL/len/header-cksum; drop+count fragment→`fragdrop`, not-for-us→`inaddrerr`, bad cksum→`badcksum`, bad ver/IHL→`hdrerr`, bad len→`badlen`; demux ICMP, `noproto` TCP/UDP stubs; **TTL parsed but NOT a delivery gate** — RFC 1122 §3.2.1.7, `ttlexp` stays 0) + `nsfip_output` (build header in the PBUF headroom, monotonic id, computed cksum, route, `dev_send`) + fixed 16-entry **routing table** from HOME (classful on-link) + GATEWAY (default), longest-match, next-hop 0 = the point-to-point peer (`nsfip_config`/`nsfip_route_add`/`nsfip_local_add`/`nsfip_is_local`/`nsfip_route`; ADR-0024). **Every header field read/written BYTE BY BYTE** (big-endian), never a struct overlay/cast; addresses are `UINT`s (octet-1 in MSB) — the CTCI-codec discipline (green-and-wrong on the little-endian host otherwise). M2-3: **NSFICM** (`src/nsficmp.c`) echo responder in **the same PBUF** — verify ICMP cksum, flip type 8→0, recompute, strip IP header (opens exactly the headroom `nsfip_output` re-prepends), `nsfip_output` with src/dst swapped; `icmp_outecho` counted only on output success (ownership + counter never double up); non-echo/bad-cksum counted + freed. Seam wired: STC `EV_PACKET_RECEIVED`→`nsfip_input`; startup calls `nsficmp_init`+`nsfip_config` AFTER interfaces register (`src/nsfmain.c`). Host **569→641** (TSTCKSUM 10; TSTIP 39 — capture-DEVOPS + literal vectors + leak gate; TSTICMP 23 — NSFHOST-loopback echo round-trip with a verified reply + bad-cksum drop + leak gate); `-Wall -Wextra -Werror` clean; NSF module cross-links clean (cc370/as370/ld370), alias scan clean (11 new unique exports `NSFIP*`/`NSFICM*`/`NSFCKSUM`, statics ENTRY=NO). §11.1/11.4/11.7 + ADR-0024 + changelog v1.19 updated. **VALIDATED LIVE on MVSCE** (STC 165, real CTCI pair 0500/0501): `S NSF`→`NSF210I/211I` up; host `ping 192.168.200.1` **the from-scratch NSF stack answers** (`ttl=64`, ~1 ms RTT), counters exact — over 1000 packets `NSFIP in/out`, `NSFICM inecho/outecho` **all 1000**, every drop counter 0. Two things fell out of the live gate: (a) **a write-completion clear-race, FIXED** — the executive reap read+cleared the write subtask's IOB ECB `wecb` while the subtask polled it, stealing the completion and stalling the write pipeline after ONE frame (the read path never had this: the read subtask owns `recb`, hands up `rpost`/`rready`); fix makes write symmetric (`wpost`/`wready`, executive never touches `wecb`; CTCIDEV 108→112; TSTCTCI sustained-write regression, 44→168). (b) the last-frame-of-burst stall, initially misread as a completion-wake issue — resolved at **issue #21**, below. Host **765/765**; TSTCKSUM/TSTIP/TSTICMP also **CC 0 on-MVS** (112/112, target byte-order proven). **Issue #21 FIXED (ADR-0025) — the M2 0-loss gate is now CLEAN.** Three separately-proven defects: (1) `nsfthr_timed_wait`/`nsfthr_join` passed `ecb_timed_waitlist` a timeout ECB NOT in the WAIT ECBLIST — the STIMER exit posted a dead stack ECB, so the "timed" wait never timed out (the CTCI 500 ms self-poll was dead code; ADR-0023 §6's "timeout does not fire on the CRT main task" was this bug misdiagnosed). Fixed: WAIT on `{target, tmo|VL}`, target ECB never cleared/phantom-posted; proven both ways by MVS-only **TSTTHRW** (old shape hangs 2 s+ until a real post; fixed fires at 500 ms on a subtask AND the main task; a join of a live subtask times out to RETAIN). (2) The §5.3 WAIT-skip never rechecked device work after the `dev->ecb` reset: `DEVIO` gains a side-effect-free `pending` probe (CTCI: `rready || (wready && txbusy)`, mirroring service's consume conditions) + `nsfdev_work_pending` (`NSFDPEND`) as a 4th `evt_set_devices` hook, consulted before the WAIT commit (host-proven with no timer running: a destroyed-wake completion reaps the same pass). (3) **The transport mechanism, isolated live after (1)+(2) deployed:** a WRITE SIO issued while the blocking READ is outstanding **queues at the IOS level (shared channel) until the next inbound frame completes that READ** — slow replies tracked the sender's interval exactly (505 ms at `-i 0.5`, 2020 ms at `-i 2`; the earlier "bimodal 200-311 ms band" was that run's ping interval, and Hercules `ctc_ctci.c` holds no lock across its read wait, so the queueing is above the device handler), and with no next frame the tail stalled forever. Fixed by **pair sequencing**: `service` marks the read release (`CTCIDEV.rhold`, 112 B unchanged), `kick` posts `returnecb` only when no WRITE is queued or outstanding — every WRITE issues with the READ parked; the un-armed window is lossless (Hercules buffers, §9.3); PBUF ownership + the kick-clocked handoff unchanged; `kick` also walks past dropped frames instead of stranding the sendq. **M3+ constraint:** a locally-originated WRITE while the READ is armed still queues — HIO or an attention-driven read protocol must land before the first M3 transmit path (documented in ADR-0025; the ping-pong throughput follow-on is conditional on the same solution). **Gate proven live** (MVSCE, real pair 0500/0501): 1000-packet ping → **1000/1000, 0 % loss, unimodal** (min/avg/max 0.550/0.918/35.1 ms, p99 < 1 ms, zero ≥ 100 ms, last frame 0.899 ms); `LNK1 in 1006 == LNK1 out 1006`, all drops 0; `P NSF` → NSF830I→NSF011I→IEF404I same-second. Host **804/804** (TSTCTCI 168→207: destroyed-wake reap, loop-consults-probe, rhold ordering); on-MVS regression **188 PASS** batch+TSO (TSTTHRW/TSTCTHR/TSTEVTM/TSTSTCM/TSTCKSUM/TSTIP/TSTICMP/TSTTMACC). Spec v1.20 (§5.3/§9.3) + ADR-0025 + ADR-0023 annotation. **M2-4/M2-5 DONE — M2 COMPLETE.** M2-4: `nsficmp_send_error` (`src/nsficmp.c`, alias `NSFICMSE`) builds the ICMP error in a FRESH PBUF (orig stays read-only/single-owner) — new IP header + ICMP type/code header quoting orig's IP header + first 8 payload bytes (RFC 792), checksummed once; suppressed per RFC 1122 §3.2.2 (error-on-error, broadcast/multicast dest, non-initial fragment, non-unicast source), no counter for a suppressed send (spec 11.7 has none). Only ONE trigger is live in v1: `nsfip_input`'s existing `noproto` path now also calls `nsficmp_send_error(orig, 3, 2)` (protocol unreachable) — port unreachable and TTL exceeded are fully built but intentionally uncalled until M4 sockets / a forward path exist (§11.2 documents why). M2-5 found the §11.7 counters + NSFTRC wiring already in place from M2-1..3; the gap was test coverage — every IP counter (incl. `out`/`noproto`/`ttlexp`==0) now reads back by name, plus a dedicated flag-on/flag-off trace test. `test/tsticmp.c` gained a CAPTURE-device scenario for the live trigger (byte-exact decode) and direct-call scenarios per suppression rule (nsfip_input's own demux filters 3 of the 4 cases before the noproto trigger, so send_error is exercised directly — with the capture route still wired so a broken guard would be caught, not silently pass). Host **804→861** (TSTICMP 23→66, TSTIP 39→53); cross-link + alias scan clean (`NSFICMSE`, no collisions). **Both live checks proven on MVSCE** (STC, real pair 0500/0501): a raw protocol-253 datagram from the host draws `ICMP ... protocol 253 unreachable` in `tcpdump` (quoting the original exactly), `errsent`/`noproto` 0→1; `F NSF,STATS` after a 20-packet 0%-loss ping shows every counter populated and consistent; `P NSF` clean same-second, no dump. Spec v1.21 (§11.2/§11.7 + changelog). **M2 COMPLETE. M3 (sockets + NSFRQE + UDP + EZASOKET) next.** |
| **M3** | Sockets + NSFRQE + UDP + EZASOKET (M3 set) | UDP echo via EZASOKET from host; leak-free. **`NSFRQE` freezes here.** | ✅ **Complete** (M3-5 live gate maintainer-countersigned, PR #34; issue #28 closed). **M3-0 done** (the CTCI locally-originated-write prerequisite the M2 gate left open). **M3-0a** (Stage 0, issue proven, ADR-0027 grounding): `test/mvs/tsthio.c` + `test/asm/tsthalt.asm` proved live that a PROBLEM-STATE program can halt an outstanding CTCI READ — IOHALT (SVC 33) *and* PURGE (SVC 16) both fire from problem state with no abend, and the guest-visible completion is **X'48'** (purged; the post code, NOT the residual, is the discriminator); a fresh READ re-arms cleanly. **M3-0b done — IOHALT active read-park (ADR-0027):** the write-kick actively parks the armed READ by IOHALT-ing it (UCB chased DCB+44→DEB+32 + UCBNAME-checked, cached at init; `NSFCIHLT` in `asm/nsfctcio.asm` uses `FUNHEAD SAVE=<static>` — executive-only caller, and a type-2 SVC needs the proper save-area linkage; the S0C1 that fell out first was a **column-72 continuation merge** dropping the `SVC 33`, CLAUDE.md §3 re-pinned). Its wait completes X'48' (purged) or X'7F' (a raced frame) → the ADR-0025 `rhold` path → WRITE issues → `returnecb` re-arms. **Three read-completion classes** + **counter split**: `ierr` = genuine device errors + resource exhaustion only; new CTCI-private `nonip` (non-IPv4/malformed codec drops) and `rpurge` (purged reads). `CTCIDEV` 112→**124 B**. Host **875→906** (TSTCTCI +31: halt-park / halt-data race / counter classes). **VALIDATED LIVE on MVSCE** (real 0500/0501): *locally-originated* — TSTCTCM CC 0 batch+TSO, on a truly idle link (`in_delta=0`) the crafted echo reached the wire with **`out=1 rpurge=1 ierr=0`** (old code would stall until the next inbound frame); *regression* — `S NSF` device up (UCB chased clean), host 1000-ping **1000/1000 0 % loss** unimodal 0.516/0.898/1.608 ms, `LNK1 in 1019 == out 1019`, all drops 0, **`rpurge 0`** across the run (receive→reply never halts — the halt fires only for locally-originated writes), `F NSF,STATS` shows the new counters, `P NSF` clean same-second, no dump. Spec v1.23 §9.3 + ADR-0027. **M3-1 done — NSFSOC socket object model + the
NSFRQE frozen contract.** Protocol-independent socket machinery, host-tested end
to end over a **test-only dummy PROTOPS** (direct calls, no event loop, no real
transport, **no new MVS seam** — sockets are not yet reachable, so there is no
live feature gate). `include/nsfreq.h`: `NSFRQE` **defined complete now** as the
phase-boundary contract (64 B core, `NSF_SIZE_ASSERT`, pool objsize 96, eye
`"RQE "`), Phase-2 fields (`ubuf`/`ulen` + SOCKCB `owner_ascb`) present so the
layout is stable for M5; `RQ_*` fn codes, `RQ_F_NONBLOCK`, and the EZASOKET
`NSF_E*` errno set (IBM/BSD numbering matching libc370, `NSF_`-prefixed to never
collide with `<errno.h>`, marked provisional — only the LAYOUT freezes, not
`errno_`). `include/nsfsoc.h` / `src/nsfsoc.c`: SOCKCB (72 B, `NSF_SIZE_ASSERT`,
≤128), the `SOCKET` pool reserved in the init window (no runtime
`mm_pool_create`), `sock_alloc` (EMFILE) / `sock_lookup` (gen-checked → EBADF on
stale/reused) / `soc_desc`, the `PROTOPS` vtable + `soc_dispatch` (pure
mechanism — invokes the op, never auto-completes; EOPNOTSUPP for a NULL op,
EINVAL for an unknown fn), the parked-request pattern (`soc_park`/`soc_complete`;
non-blocking → EWOULDBLOCK; a parked NSFRQE is socket-owned until completed), and
**`soc_destroy`** — the ONE teardown checklist (detach → flush rxq/acceptq PBUFs
→ complete every parked NSFRQE with `NSF_ECONNABORTED` → release pcb → **bump the
SLOT generation** → `mm_free`) every close/reset/shutdown path calls.
**Refinement (spec §10.2, v1.24):** the generation lives in the **table slot**,
not only the SOCKCB, or §10.5's "bump gen, mm_free" is a no-op and a reused slot
would hand back an old descriptor; SOCKCB.gen is a mirror. **`soc_complete`'s
app-ecb POST is a same-AS plain POST over the existing thread seam**
(`nsfthr_post`, real SVC 2 on MVS) — NOT `nsfevt_plat_post` (which only sets the
bit on MVS and would never wake an app task WAITing solely on `rqe->ecb`);
cross-AS wakeup stays M5/Phase 2 (ADR-0022), NSF stays unauthorized/problem-
state. **NSFRQE freezes at the M3 exit gate.** Host **906→979** (TSTSOC **73**:
table+EMFILE, descriptor stale-fd/reuse guard, dispatch of all 8 callbacks +
EOPNOTSUPP/EINVAL, park/complete + non-blocking, destroy leak gate + per-pend-
slot sweep); `-Wall -Wextra -Werror -pthread` clean; cross build
(cc370/as370/ld370) links clean; alias scan clean (12 unique `NSFSO*`, verified
in `cc370 -S` + a global header grep). **Not wired into the STC and NOT in the
`NSF` load module** — sockets are unreachable, `S NSF` is byte-for-byte unchanged
(no live socket feature claimed). Spec v1.24 §10.2/§10.5 + changelog. **M3-2
done — NSFREQ request transport + fn dispatcher; NSFRQE FROZEN.** The Phase-1
backbone the socket API rides on. **Transport** (`src/nsfreq.c`): an
in-address-space request queue (the NSFXQ CS-safe handoff, so app subtasks on
other TCBs enqueue lock-free) + the §5.3 `requestECB`. App side —
`nsfreq_submit` = `xq_push` + `nsfthr_post(requestECB)` (a real SVC 2 POST, ONE
call site → M5 swaps for cross-AS), then WAIT on the request's own ecb
(`nsfreq_wait`/`nsfreq_call`). Executive side — `nsfreq_drain` **resets
`requestECB` BEFORE draining, then double-checks (drain; dispatch; loop while
non-empty)**; the loop's WAIT-gate rechecks `nsfreq_pending` before committing to
WAIT (ADR-0022 reset-before-WAIT + double-check-drain — the #27 lost-request
class, in PRODUCTION not just a test; drain terminates because a blocking app has
≤1 outstanding request). **Wiring** (`src/nsfevt.c`): `evt_set_request(ecb,
drain, pending)` mirrors `evt_set_operator`/`evt_set_devices` — `requestECB`
joins the ECBLIST (between the device ECBs and the cibECB), the pending probe
joins the WAIT gate, the drain runs each pass; inert (no ECBLIST/gate/behavior
change) until `evt_set_request` is called, so the `NSF` load module is unaffected.
**Dispatcher** — the COMPLETE frozen verb set: INITAPI/TERMAPI/SOCKET/BIND/
GETSOCKNAME/CLOSE handled in NSFREQ; CONNECT/LISTEN/ACCEPT/SEND/SENDTO/RECV/
RECVFROM/SHUTDOWN delegate to `soc_dispatch` (op completes/parks; real UDP M3-3,
TCP M4); SELECT/SET|GETSOCKOPT/FCNTL/GETPEERNAME → `NSF_ENOSYS`; unknown fn →
`NSF_EINVAL` (never a fall-through/crash). **App registry** — INITAPI token
`(gen<<16)|idx` (→ `apptok`), RQ_SOCKET stamps `owner_ascb`, RQ_TERMAPI mass-
teardown via new `soc_foreach` (`NSFSOFEA`) + `soc_destroy` (parked req →
`NSF_ECONNABORTED`). **Contract change (the "decide now" moment):** `NSFRQE`
`rsvd[2]` → **`apptok`(@56) + `rsvd`(@60)** — 64-byte core UNCHANGED
(`NSF_SIZE_ASSERT` holds), so the freeze holds; a named use of reserved space, not
a layout break. **NSFRQE FROZEN at M3-2** (changing it now needs an ADR). **A
latent host-shim UAF fixed** (`src/nsfthr_host.c`, host-test only, MVS
unaffected): `nsfthr_post` dereferenced the ECB a second time (via
`nsfevt_plat_post`) after the first broadcast could release a waiter — fine for
CTCI ECBs (persistent NETDEV storage) but a use-after-free once an app's STACK
NSFRQE is freed the instant `nsfreq_call` returns (found via a SIGSEGV backtrace
loop; fixed by holding the mutex across both derefs). Host **979→1052** (TSTREQ
**73**: dispatch coverage incl. ENOSYS/unknown/EBADF/EPROTONOSUPPORT, TERMAPI mass
teardown + leak gate, app-registry EMFILE, host pthread round-trip [blocked-not-
spun, woke-exactly-once], lost-request stress); `-Wall -Wextra -Werror -pthread`
clean. **Stability gate (#27):** round-trip + lost-request **200× sequential +
100× single-core (`taskset -c 0`) on Linux, 0 failures** (the exposer macOS
hides). Cross build (cc370/as370/ld370) links clean (29 modules); alias scan
clean (`NSFRQ*`/`NSFSOFEA`/`NSFEVRQ` unique, statics ENTRY=NO); no runtime alloc
after `mm_init_complete`. **VALIDATED LIVE on MVSCE:** the same-AS round-trip
**TSTREQM CC 0 (batch + TSO), 30 PASS** — a cthread app subtask does INITAPI
(tok `00010000`)→SOCKET (desc `00010000`)→CLOSE→TERMAPI over the real
queue+`requestECB` (ecb_post SVC 2 / ecb_wait SVC 1 on a separate TCB), attach/
detach on the executive, leak gate clean; focused regression **CC 0, 560 PASS**
(TSTREQ/TSTSOC/TSTEVTM/TSTCTHR/TSTTHRW/TSTSTCM/TSTCKSUM/TSTIP/TSTICMP/TSTTMACC —
no regression from the `nsfevt.c`/`nsfsoc.c` changes). **NO user-visible
feature** (sockets unreachable until M3-3); `S NSF` not redeployed (the NSF module
is functionally unchanged — the wiring is inert — and the STC machinery is
regression-proven on MVS by TSTSTCM/TSTEVTM). Spec v1.25 §10.4/§10.5 + changelog.
**M3-3 done (host + cross-link; live gate pending) — NSFUDP: datagram in/out,
port demux, checksum; sockets reachable end to end.** `src/nsfudp.c` +
`include/nsfudp.h` (`UDPPCB` 20 B/objsize 64; bind/ephemeral/EADDRINUSE; demux
specific-laddr-beats-ANY; `nsfudp_input`→rxq/parked-RECV; RQ_SENDTO→`nsfip_output`;
RQ_RECVFROM copy+peer+**datagram-truncation**; `UDPADDR` 8 B rxq record; real
PROTOPS; `nsfudp_reserve`/`_init`/`_protops`). **Checksum decision (ADR-0028):**
pseudo-header via a SEED — `in_cksum` split into `in_cksum_partial`+`in_cksum_fold`
(`in_cksum ≡ fold(partial(…,0))`, M2 vectors byte-identical), NOT an overlay (the
input PBUF has no headroom); RFC 768 zero-cksum BOTH ways (out 0→0xFFFF, in 0→
accept). **IP demux seam (ADR-0028):** `nsfip_register_proto` — REQUIRED (not an
explicit `case`) so `nsfip.c` links into the `NSF` module without `nsfudp.c` (UDP
unreachable until EZASOKET); ICMP stays direct. **Port unreachable** = M2-4's
first live trigger (`nsficmp_send_error(orig,3,3)` over the untrimmed datagram,
then free — no double-free). New provisional errnos (`NSF_EADDRINUSE`/`EMSGSIZE`/
`EHOSTUNREACH`/`EDESTADDRREQ`) — values only, NSFRQE layout freeze intact. Host
**1052→1197** (TSTUDP 142: literal 0x9371 output vector + zero→0xFFFF + input
accept/reject; bind/demux all cases; input→rxq→RECVFROM + parked-RECV-on-arrival +
rxq-full-drop + oversized-truncate; port-unreach quoted+free-once; SENDTO byte-
asserted framing; soc_destroy leak gate; TSTCKSUM 10→13 seed vector). Host-only
threaded loopback round-trip (a SENDTO completing a *parked* RECVFROM) — lock-
stepped to 1 datagram in flight + a sender-waits-for-BIND barrier (a bursting or
early sender legitimately overruns the §3 bounded queues / draws port-unreachable
— correct behaviour, not a stack bug); **500× sequential macOS clean** (Linux
single-core **on Linux, 0 failures/0 hangs**). `-Wall -Wextra -Werror -pthread`
clean; cross build (`NSF` module + 31 test modules) links clean; alias scan clean;
no runtime alloc after seal; `UDPPCB`/`UDPADDR` size asserts hold. NSFUDP NOT in
the `NSF` module (M3-2 precedent). Spec v1.26 §12.4/§11.2 + ADR-0028 + changelog.
**VALIDATED LIVE on MVSCE** (`test/mvs/tstudpm.c`, full stack over the real
0500/0501 pair, `-DNSFCTCI_CUU`, cthread app subtask on the request path): TSTUDPM
**CC 0 batch+TSO**, all three scenarios — RECEIVE (`nc -u` → RECVFROM rc=2, peer
`192.168.200.2:<eph>` correct); LOCAL SEND (SENDTO rc=8 / out 1, **byte-perfect in
`tcpdump`**: `.1.7777 > .2.9`, UDP len 16, non-zero cksum `0xA129` [pseudo-header
seed on the target], payload `"UDP-hi!\n"`, sent promptly at device-up — the
ADR-0027 IOHALT read-park, **no #28 abend, no dump**); PORT UNREACH (unbound port →
`noport`+`errsent`+**ICMP port-unreachable in `tcpdump` quoting the original**).
**1000-ping ICMP regression on the redeployed `NSF`** (refactored `nsfip.c`) still
**1000/1000 0 % loss unimodal 0.554/0.876/1.735 ms**, all drops 0. `TSTCKSUM`/
`TSTIP`/`TSTICMP`/`TSTREQM`/`TSTUDPM` **CC 0 on MVS** (0x9371 seed vector big-endian
on S/370; M2/M3-2 regression clean). **M3-3 COMPLETE.** **M3-4 COMPLETE (host +
cross-link + on-MVS VALIDATED live) — NSFEZA: the EZASOKET API layer.**
A surface-neutral core + two facades (ADR-0029). **C API** (`src/nsfeza.c` /
`include/nsfeza.h`, the `@@NS*` alias namespace disjoint from libc370 dyn75
`@@75*`): `nsf_initapi`/`socket`/`bind`/`sendto`/`recvfrom`/`close`/
`getsockname`/`termapi` + `nsf_lasterrno` + the EZASOH03 plist decoder
`nsf_ezasoh03` — each builds an NSFRQE, `nsfreq_call`s it, maps RETCODE/ERRNO;
**halfword 0-based socket numbers** on a per-app mapping table onto the internal
`(gen<<16)|id`, MAXSOC clamped to the pool limit (64)/MAXSNO=clamped-1, implicit
INITAPI, EBADF-after-CLOSE, TERMAPI mass teardown; `sockaddr_in` read/written
**byte-wise** (network order) so host and target agree (the M2 discipline).
**EZASOH03 facade** (`asm/ezasoh03.asm`) — a THIN veneer that hands its R1 plist
to `@@NSOH03`; **PDPPRLG not FUNHEAD** (Mike suggested FUNHEAD, but primary
source — the cc370 C prologue reads the caller's DSANAB@76(R13), FUNHEAD never
sets it → save-chain corruption/issue-#8; the analog is libc370's HAND-WRITTEN
asm VSAM exit stubs `src/clib/@@vsopen.c` EODAD/LERAD/SYNAD — PDPPRLG +
`L R15,=V(@@VSXEOF)` + BALR; gives a per-invocation DSA, concurrency-safe, no
static save area). Companion macro `maclib/nsfezasm.mac` adds SENDTO/RECVFROM (new
codes **SNDT/RCVF**; Shelby's first-4-char scheme collides SEND/RECV). **Errno
fix:** `NSF_ENOSYS 78` was wrong (Table 67 has no ENOSYS; 78 is EDEADLK) →
stub verbs now `NSF_EOPNOTSUPP` (45), `NSF_ENOSYS` deleted+tombstoned. **#28
stays OPEN** (IOHALT with no outstanding READ, now reachable via app sends): NO
abend (Hercules `ctc_halt_or_clear` no-ops unless `fReadWaiting`), but NOT
harmless on the guest side — no X'48' purge means `service` never sets `rhold`
(set only on a read completion), the re-armed read blocks on the idle link, and
the WRITE STALLS until the next inbound (the pre-#21 stall class). Narrow window
(a burst keeps sendq full → no drain → no race), so no live run hit it; real fix
= a `rarmed` guard (IOHALT only with a read provably outstanding; else the
channel is free → WRITE direct) in a dedicated PR. Host **1197→1261**
(TSTEZA 64, `-Werror` clean, 11 unique `@@NS*` aliases no collisions); full
cc370/as370/ld370 cross-build of all 34 test modules links clean incl. the
EZASOH03 asm↔C boundary. NSFEZA links into the APPLICATION (like nsfreq.c's app
side) — the `NSF` `[[module]]` source list is UNCHANGED. **VALIDATED LIVE on
MVSCE** (real CTCI pair 0500/0501): **TSTEZAH CC 0 batch+TSO** — the asm-veneer
seam under Mike's exact predicted-failure conditions: Phase A `calls=50/50
bad_r15=0 bad_rc=0 bad_errno=0` (2 concurrent cthread subtasks × 50 consecutive
calls, R15=0 + RETCODE=-1 + ERRNO=45 every call, no abend), Phase B full
lifecycle through the veneer (INIT maxsno=63, SOCK/BIND/GETS/CLOS rc=0, GETS
returned the bound addr+port), leak gate clean. **TSTEZAM CC 0 batch+TSO** — the
C API over the real stack: `NSF210I CTCI 0500/0501 UP`, INITAPI rc=0/maxsno=63,
SOCKET fd=0 (0-based), BIND rc=0, **SENDTO rc=8** (the local send through
nsf_sendto + the ADR-0027 IOHALT read-park; this send halted an OUTSTANDING read
— the normal path, NOT the #28 race), TERMAPI rc=0,
leak gate clean. **A live-only bug found + fixed (the reason TSTEZAH earns its
keep):** the first run S0C4'd because an inline comment reaching **column 72**
on the veneer's `LR 11,1` line made as370 swallow the next `LA 1,88(,13)`
(CLAUDE.md 3 — invisible to the host build AND a clean cc370/as370/ld370 link;
only the live dump showed it). Fixed by keeping instruction-line comments short.
Spec v1.27, ADR-0029 amended, conformance doc §2.1/§3. **M3-5 done (host +
cross-link + live gate green) — NSFECHO UDP echo sample + host client + issue
#28 closed (ADR-0030); the M3 exit gate.** The first user-visible NSF program:
`samples/nsfecho.c` (its own `[[module]]` NSFECHO carrying the Phase-1 stack —
bring up CTCI+IP+UDP, `nsf_initapi`/`socket`/`bind`, a **blocking**
`nsf_recvfrom`→`nsf_sendto` echo loop on an ATTACHed subtask, raw-byte `QUIT`
sentinel per spec 15.3, shutdown leak gate + stat dump; PARM = port via
EBCDIC-aware `atoi(argv[1])`, device hardcoded via named constants since
`[build].cflags` is global) + `samples/host/echo_client.py` (stdlib,
echo/sizes/kill9/quit/gate, each PASS/FAIL + exit code) + `jcl/NSFECHO.jcl`.
**The sample reproduced #28 at scale and it was NOT harmless** (my earlier fence
was wrong): on an idle link a locally-originated echo reply was held in the CTCI
write path until the next inbound frame (wire-proven 2 s stall; `echo`+`ping`
1000/1000; `rpurge` 39/300). Root cause: `kick` IOHALT-parked the read on
`!rhold`, which doesn't prove a READ is armed — a send in the arming window
halted an un-armed read (Hercules no-ops it), no X'48', `rhold` never re-set,
WRITE stalled. **Fixed (ADR-0030, folded into this PR at Mike's direction): the
`rarmed` guard** — `read_sub` sets `CTCIDEV.rarmed` after `ctci_read` (cleared
after the completion; sole writer, executive read-only) + POSTs `dev->ecb` after
arming; `kick` IOHALTs only when `rarmed`. `CTCIDEV` 124→128 B; TSTCTCI scenario
10 updated (the fix correctly withholds the no-op halt when data already
completed the read — the halt-IS-requested path stays covered by
`scenario_local_write_halt`/`scenario_send_write`). Host **1261→1262**, `-Werror`
clean; NSF+NSFECHO+33 test modules cross-link clean at 128 B; alias scan clean
(NSFECHO exports only `main`, helpers static). **VALIDATED LIVE on MVSCE** (real
0500/0501): idle-link `echo` **1000/1000** (no ping — was 21/1000 before), full
`gate` green, `echoed=2434 send_fail=0`, **`rpurge` 39/300→2391/2434 (98%)**,
`ierr=0`, leak clean, **CC 0**, no dump; M2 ping regression **1000/1000 unimodal**
(0.560/0.922/1.865 ms) — receive path unaffected. Spec v1.28 (§9.3 + M3 exit
gate + changelog) + ADR-0030. **M3 exit gate GREEN, maintainer-countersigned
(PR #34 merged) — M3 COMPLETE. M4 (TCP + EZASOKET M4 set) next.** |
| **M4** | TCP (state machine, data path, rexmit) + EZASOKET (M4 set) + loss harness | telnet TCP echo, clean FIN, survives 5% loss; TIME_WAIT reclaim shown | ✅ **Complete** — **M4-1 done (host + cross-link; live gate is M4-2's).** NSFTCP skeleton (`include/nsftcp.h`/`src/nsftcp.c`): the TCB (§13.2 verbatim, 188 B on target `NSF_SIZE_ASSERT`; pool slot `max(sizeof(TCB),256)` — 256 target growth reserve, exact struct size on the host where 8-byte pointers + four 48-byte TMRs inflate it to 328 B, so a fixed 256 overflows the slot: the `sizeof(SOCKCB)` lesson, **caught live by ASan**), the `TCP_SEQ_LT/LEQ/GT/GEQ` signed-difference macros (pinned first, wrap vectors), and `tcp_input` written to read line-by-line against RFC 793 pp. 64–76: validate length/data-offset/**mandatory** checksum (ADR-0028 pseudo-header seed proto 6, **no** zero-checksum exemption) → demux (4-tuple then listener) → no TCB → **RST per RFC 793 §3.4** (`tcp_output_rst`, the one live emitter) → matched TCB → RFC-ordered per-state stubs (drop+count, unreachable in M4-1 since nothing leaves CLOSED). **RST seq/ack byte-exact** (the crux everyone gets wrong): SYN → `<SEQ=0><ACK=SEG.SEQ+SEG.LEN><RST,ACK>` with SEG.LEN counting the SYN (bare SYN → ack = seq+1); ACK → `<SEQ=SEG.ACK><RST>`; never a RST-on-RST (counted `resetrcvd`); and — RFC 1122 §3.2.1.3 — never a RST toward a non-unicast source (`nsfip_input` checks only the DEST, so TCP filters the source, mirroring `nsficmp_send_error`; a review-workflow find). `tcp_destroy` = the §13.4 checklist from day 1 (4× `tmr_cancel`, free sndq/oooq, unlink, detach SOCKCB — soc_destroy owns parked-request completion, `mm_free`). PROTOPS surface = `tcp_attach` (alloc TCB, `s->pcb` left NULL on failure so soc_create→soc_destroy→detach no-ops) + `tcp_detach`; every other verb NULL → dispatcher completes EOPNOTSUPP (the UDP precedent). All 12 §13.5 counters + private `hdrerr`. Mirrors NSFUDP: registration seams (`nsfip_register_proto(6)`/`nsfreq_register_proto(6)`), **NOT in the `NSF` load module** (unreachable until the EZASOKET M4 set — inbound TCP still draws ICMP protocol-unreachable in production; `S NSF` byte-for-byte unchanged). Host **1262→1399** (TSTTCP 137: seq wrap, the independent checksum vector 0x22F4 two-sided, RST byte-exact per §3.4, non-unicast-source suppression, bad-cksum/bad-offset/runt drops, socket/TCB lifecycle + EMFILE-class exhaustion + leak gate; no threads — the M4-1 contract); `-Wall -Wextra -Werror -pthread` + ASan clean (ASan caught the pool-objsize overflow); NSF+NSFECHO+34 test modules cross-link clean; alias scan clean (5 unique `NSFTC*`). `test/mvs/tsttcpm.c` (host=false) re-runs the byte-order vectors on S/370 — **CC 0 batch+TSO on MVSCE** (TCB SIZE_ASSERT fires at cross-compile). Spec v1.29 (§13 status note + changelog). **M4-2 (handshake + teardown + TIME_WAIT) next.** **M4-2 done (host + cross-link; live gate `tsttcph.c` pending countersign) — the TCP connection machine (ADR-0031).** The M4-1 RST-only skeleton becomes a full state machine: **active open** (`tcp_connect`: ISS from `nsf_now`, SYN+MSS, SYN_SENT, park RQ_CONNECT; SYN|ACK → ESTABLISHED completes the parked connect; RST → `NSF_ECONNREFUSED`), **passive open** (`tcp_listen` clamps backlog to `NSFSOC_ACCEPTQ_MAX`; inbound SYN → an EMBRYONIC child SOCKCB+TCB in SYN_RCVD linked to the listener; final ACK → ESTABLISHED, queued on the listener's `acceptq`; `tcp_accept` hands back the child's descriptor — pool exhaustion at SYN reclaims the oldest TIME_WAIT first, else drops the SYN), and full **FIN teardown** (active FIN_WAIT_1→2→TIME_WAIT; passive CLOSE_WAIT→LAST_ACK→CLOSED; simultaneous CLOSING→TIME_WAIT), **TIME_WAIT** = 2MSL (60 s, `t_2msl`) + oldest-first pool reclaim (`twreclaim`). Handlers read in **RFC 793 pp.64-76 step order** (seq check → RST → SYN → ACK → text → FIN); the M4-1 macros are the only sequence comparisons. **Three design decisions (ADR-0031):** (1) an established un-ACCEPTed child hangs on the acceptq via a **second** TCB `QELEM` (`acceptlink`) + a `listener` back-pointer — SOCKCB untouched (72 B) — and `soc_destroy`'s M3-1 acceptq-as-PBUF flush is removed (the acceptq is protocol-owned, drained by unlinking; a TCB freed as a PBUF was a latent heap corruptor). (2) **Background close = ownership inversion:** `close()` completes the request immediately (BSD semantics) and finishes in the background; end-of-life drives `soc_destroy(sock)` → `tcp_detach` → `tcp_destroy`, and **`tcp_destroy` NEVER calls `soc_destroy`** (the recursion break). A RST in a synchronized state completes the parked request with the SPECIFIC errno (`NSF_ECONNRESET`) BEFORE `soc_destroy` (else it is re-completed `ECONNABORTED`). (3) **`accept` is the TRAILING `PROTOPS` member** (C zero-fills omitted trailing initializers → UDP + every M3 test dummy compile untouched) + an `RQ_ACCEPT` case in `soc_dispatch`; `RQ_LISTEN` gets a synchronous `do_listen` (the r-less listen op cannot ride `do_delegate`'s "op completes r" contract — a latent gap since M3, TCP's real listen is the first rc==0). **Close-op return convention:** a close op that owns `r` returns **`NSF_CLOSE_OWNED`** (-2); `do_close` then does nothing — any other return (0) runs the default `soc_destroy`+complete, so every M3 dummy `d_close` (returns 0) keeps working unchanged (this is what the `do_close` delegation demands — a dummy that returned 0 and did nothing had hung the app thread). Five new provisional errnos (`ECONNRESET 54`/`EISCONN 56`/`ENOTCONN 57`/`ETIMEDOUT 60`/`ECONNREFUSED 61`, Table 67). **Counter-name trap:** an `STSCTR` name is a **12-char field** (nsfsts.h), so the spec-§13.5 concept "timewaitreclaim" (15) is registered — and read — as **`twreclaim`**; the full name truncates to "timewaitrecl" and is then unreadable by `sts_value` (M4-2 is the first to tick+read it). **No data path (M4-3):** ESTABLISHED drops payload + counts `datadrop`, RCV.NXT never advanced over undelivered data. **No rexmit (M4-4):** the CTCI link is lossless (M2 gate), so a lost segment fails the connection; no RTO half-built. Host **1399→1641** (TSTTCP 137→379: handshake byte-exact both directions, teardown matrix, TIME_WAIT expiry+reclaim, RST→ECONNRESET/ECONNREFUSED, malformed-option drops, leak gate per scenario); `-Wall -Wextra` clean; TCB 188→200 B (`NSF_SIZE_ASSERT`; the self-referential `listener` field is `struct tcb *`, not the `TCB` typedef — undefined inside its own struct). NSFTCP still OUT of the `NSF` load module (unreachable until the EZASOKET M4 set, M4-5). **VALIDATED LIVE on MVSCE** (real 0500/0501, `test/mvs/tsttcph.c`, cthread app subtask): **TSTTCPH CC 0 batch+TSO** — both handshake directions + FIN teardown on the wire (host `tcpdump -ni tun0`). PASSIVE: `nc </dev/null 192.168.200.1 3000` → guest **SYN,ACK carrying `options [mss 1460]` win 4096** → ACK (3WHS), then a clean 4-way FIN (guest ESTABLISHED→CLOSE_WAIT→LAST_ACK→CLOSED). ACTIVE: the guest's **idle-link locally-originated SYN** (ephemeral 49152, `mss 1460`, win 4096 — the ADR-0030 rarmed read-park, reached the wire promptly) → `nc -l 3001` SYN,ACK → guest ACK (**CONNECT rc=0**). `established=2` (passive child + active conn), `activeopen=1`/`passiveopen=1`, `resetrcvd=0`; leak gate clean (no sockets left, TCPTCB pool at baseline); no dump. Queued SYNs from timed-out `nc` attempts before LISTEN came up drew the correct §3.4 closed-port RST live. **Live-run lessons:** a leftover **NSF STC holds the CTCI pair** (`S99 ERR 0214` on `dev_start` → `P NSF` to free 0500/0501); TERMAPI/shutdown abortively tears down an in-flight FIN_WAIT_1 connection (the job ends before the active-close FIN completes — a test-timing artifact, leak-gate-clean, not a state-machine bug). Spec v1.30 (§13 status + changelog) + ADR-0031 + conformance §3. **Live gate GREEN — maintainer merge countersign (PR #37 merged).** **M4-3 done (host + cross-link + live gate GREEN) — the TCP data path (ADR-0032).** ESTABLISHED now carries payload. **Send = copy-on-transmit:** `sndq` holds the app data (front byte = SND.UNA, bounded by `NSFTCP_SNDBUF` 4096); every (re)transmission COPIES the slice into a FRESH wire PBUF for `nsfip_output` (`tcp_sndq_slice`/`tcp_data_emit`), so PBUFs stay single-owner (§3.4) and a transmit failure loses only the copy — SND.NXT unmoved, data retained on the sndq, retried on the next ACK/window/SEND event (the seam M4-4's RTO plugs into; why no RTO is needed on the lossless link). **Receive = trim-in-place:** the inbound PBUF is `buf_trim_head`/`buf_trim_tail`'d to the in-order payload and queued on the socket rxq AS-IS (ownership transfers), so **`nsftcp_input`'s `buf_free(b)` is now CONDITIONAL** on an ownership flag threaded down the handler chain (`tcp_state_input`/`tcp_synchronized_input` take `PBUF *b, int *kept`), set to 1 ONLY on a successful `q_enq` — the double-free/leak gate (ASan/UBSan-clean; the coherence dividend: a dropped segment doesn't advance RCV.NXT, so a same-segment FIN is out-of-order and the peer retransmits data+FIN together). **Sliding window:** usable send window `(INT)(SND.UNA+SND.WND−SND.NXT)` **signed** (a shrunk window is a clean zero → pause, persist is M4-4); `rcv_wnd` is the LIVE advertised window (base − queued, right edge never shrinks); a recv that reopens the window (from 0, or by ≥ 1 MSS) sends a **pure window-update ACK** — the rule that keeps a >window transfer from deadlocking. **FIN-after-data:** `tcp_close` no longer emits the FIN inline — it sets `TCB_F_FINQ` (+state) and `tcp_output` emits the FIN once the sndq drains, guarded by `TCB_F_FINSENT` so it fires ONCE (the equality goes true again when the peer's ACK advances SND.UNA up to SND.NXT — the M4-2 teardown regression that FINSENT fixes); empty sndq → FIN same pass, byte-identical to M4-2. **EOF** = `TCB_F_RCVFIN` (peer FIN): recv on empty rxq returns rc=0, sticky, data-before-EOF ordered. **Parked SEND lives in `SOCKCB.pend_send`** (72→76 B, `SOC_PEND_SEND=3`, progress cursor in `r->p3`) — a peer of pend_recv so `soc_destroy`/`tcp_do_reset` complete it uniformly (no app hangs); NOT the TCB, because the no-hang guarantee must be impossible to forget (advisor call). Counters: `datadrop` narrowed to text dropped in a non-receiving state, `oooseg`/`dupack` tick for real, **private `rxfull`** for an rxq-PBUF-bound drop. **No oooq reassembly / Nagle / delayed ACK (M5); no RTO (M4-4).** TCB unchanged (200 B — only new flag bits, no fields). Host **1641→1791** (TSTTCP +150: segmentation seq/size + partial-ACK head-adjust, 1-MSS + zero-window + window-update flow control, blocking-send-over-budget park→drain→complete, in-order recv byte-exact, retransmission-overlap head-trim, pure-dup ACK-only, gap oooseg+dup-ACK, rxq-window-fill→drain→window-update-ACK, EOF matrix, copy-on-transmit recovery via a forced dev-down, ENOTCONN/EWOULDBLOCK); **ASan+UBSan clean** on TSTTCP (the buffer-slicing gate); cross build (NSF + NSFECHO + 36 test modules) links clean, alias scan clean (every new nsftcp helper is `static` — no new externals). NSFTCP still OUT of the `NSF` load module (unreachable until M4-5); `S NSF` byte-for-byte unchanged. `test/mvs/tsttcpd.c` (host=false) = the live gate. **VALIDATED LIVE on MVSCE** (real 0500/0501, batch CC 0, 22/22): conn #1 small ECHO (`nc` line back byte-exact, `recv=16 sent=16`, guest `[P.] len 16 win 4096` in tcpdump, clean FIN); conn #2 one-way DRAIN of 16 KB in 1 KB slices (`n=16384`, checksum matches host = byte-exact) where tcpdump shows the guest window closing to `win 0` then the **pure window-update ACK reopening it** (`ack 4097, win 0`→`ack 4097, win 1024`, same ack — the deadlock rule live); `oooseg`/`dupack`/`rxfull`/`datadrop` all 0, leak gate clean. Live-run lessons: the **NSF STC (no TCP in the module) holds CUU 0500 and answers a SYN with ICMP protocol-unreachable → the host kernel caches it and `connect` fails locally with ENOPROTOOPT** — `P NSF` frees the pair AND clears the responder ([[nsf370-ctci-write-tail-stall]] neighborhood); a **`-D` added to `[build].cflags` needs the source touched** or MBT reuses the stale `.o` (the M3-4 stale-.o gotcha — the first run silently used the device-free skeleton); the **TSO re-run of the single physical pair back-to-back stalls MIH-pending** (NO RC — the batch run is the gate, TSTCTCM precedent). Spec v1.31 (§13 status + changelog) + ADR-0032 + conformance (ENOTCONN/EWOULDBLOCK uses). **M4-4 (RTO/rexmit + loss harness) next.** **M4-4 done (host + cross-link; live gate `tsttcpr.c` pending countersign) — retransmission (fixed RTO + exponential backoff) + zero-window persist probes (ADR-0033).** The M4-3 copy-on-transmit seam becomes loss-surviving: only timers + policy, no new buffer machinery. **One reconcile choke-point** `tcp_timers_update` runs after every event that moves SND.UNA/SND.NXT/SND.WND/sndq (tcp_output tail, the SYN emitters, tcp_process_ack) and arms **exactly one** timer: rexmit while sequence space is in flight (SND.UNA<SND.NXT — data/SYN/FIN uniformly), persist while paused on a zero window with nothing in flight, else neither. **Rexmit⊕persist are NEVER armed together** — the invariant that makes the give-up teardown safe to free the TCB from a timer callback (nsftmr_run IDLEs the firing timer first; no *sibling* timer of this TCB is queued — persist off when rexmit fires, keep is M5, 2msl is TIME_WAIT-only with no unacked data — exactly `tcp_2msl_expire`'s reasoning). **Retransmit ONE segment** at SND.UNA on expiry (RFC 1122 §4.2.3.1 go-back-N — never re-blast the flight): a SYN/SYN|ACK via the control path (seq=ISS=SND.UNA), or `min(mss,sndq_bytes)` data copied from the sndq (`tcp_data_emit`, off 0), or the FIN — byte-identical payload+seq, SND.NXT never moved. **FIN retransmit** re-emits at the FIN's own seq (SND.UNA) via new `tcp_emit_seq`, `TCB_F_FINSENT` STAYS set / SND.NXT NOT re-incremented (the flag means "occupies sequence space", not "sent once"). **Fixed RTO** = `NSFTCP_RTO_TICKS` (10=1s) `<< backoff` capped at `NSFTCP_RTO_MAX_TICKS` (640=64s, shift clamped at 6 vs UB); `srtt`/`rttvar` stay 0 (**Karn+adaptive RTT is M5**). The two mutually-exclusive timers **share** `backoff` (expiry count = interval shift) + `rto`. **Give-up** after `NSFTCP_RTO_MAXTRIES` (8) no-progress expiries → `tcp_conn_abort(tcb, NSF_ETIMEDOUT)` (completes every parked req with ETIMEDOUT, pend slot cleared first — the `tcp_do_reset` pattern refactored into a shared helper — then the ADR-0031 end-of-life path); SYN_SENT give-up = the classic connect timeout, SYN_RCVD reclaims the embryonic child. A progress ACK resets backoff/rto to base + cancels t_rexmit so the reconcile re-arms fresh (RFC restart-on-ACK). **Persist** probes ONE byte beyond the window at SND.NXT **without advancing SND.NXT** (byte stays on the sndq, sent for real on window-open — keeps the "nothing in flight" bookkeeping so persist stays governing), counts `wndprobe`, backs off, re-arms. **Persist never gives up while the peer answers** — new flag `TCB_F_PROBEACK` (set on any sequence-acceptable inbound segment; a zero-window ACK proves liveness) gates the give-up: backoff still GROWS per probe (intervals visibly back off in tcpdump), but give-up (`backoff>=MAXTRIES`) only fires when PROBEACK is clear (an all-silent peer) — documented choice, ADR-0033. **Fast retransmit is M5** (`dupacks` counts a dup-ACK run, nothing acts). **TCB unchanged (200 B)** — the RTO-state fields pre-existed; only one new flag bit. **No new external symbols** (every M4-4 helper `static`). NSFTCP still OUT of the `NSF` module; `S NSF` byte-for-byte unchanged. Host **1791→1954** (TSTTCP 529→692: lost-data one-segment-rexmit byte-identical + re-arm-for-remainder, backoff-to-cap + MAXTRIES→parked-SEND-ETIMEDOUT, SYN loss active [byte-identical incl MSS]+connect-give-up, **establishment-resets-backoff** [a retransmitted SYN must not carry its grown backoff into the data phase — reset in `tcp_enter_established`, the convergence point of both handshake paths; an advisor find], SYN|ACK loss passive [child reclaimed], FIN loss [SND.NXT not re-incremented], persist probe/backoff/window-reopen [wndprobe exact], partial-ACK-resets-backoff, rexmit⊕persist invariant, **flowctl-no-oversend** [a latent M4-3 bug FOUND LIVE: `tcp_process_ack` advanced SND.UNA and re-clocked the sender via send_resume BEFORE `tcp_update_window`, so advancing past a SHRUNK window slid the right edge and the guest transmitted beyond it — on the wire a segment past the peer's window, dropped, retransmitted (rexmit not persist); fix = window-update-before-transmit (RFC 793 p.72 step 5); hides on host until the PARKED-send path (send_resume early-returns when pend_send NULL) meets a shrinking window]); **ASan+UBSan clean** on TSTTCP (`-Wall -Wextra -Werror`); NSF+NSFECHO+37 test modules cross-link clean, alias scan clean (no new externals). `test/mvs/tsttcpr.c` (host=false) = the persist live gate (guest streams → a tiny-`SO_RCVBUF` host receiver stops reading → win 0 → guest respects the window [the over-send fix] + fires a one-byte probe + completes the 4 MB on reopen; `wndprobe>0`, `rexmit==0`); `test/mvs/tsttcpd.c` gains the M4-3 regression `rexmit==0` assertion. **Genuine-loss RTO firing cannot be induced on the lossless CTCI link without root — the RTO/backoff/give-up path is HOST-proven; the drop/dup/reorder matrix is M4-6.** Spec v1.32 (§13.1 + §13 status + changelog) + ADR-0033 + conformance (ETIMEDOUT now live). **Live-driven on MVSCE (STC stopped to free the pair, then restarted):** TSTTCPD M4-3 regression batch CC 0 with `rexmit==0` (echo + 16 KB drain over `ncat`); TSTTCPR persist batch CC 0 (tiny-`SO_RCVBUF` receiver → win 0 → the guest respects the window, a one-byte zero-window probe on the wire, 4 MB completes; `wndprobe>0`, `rexmit==0`). **BUT the multi-tick backoff CADENCE is distorted live** — probe #2 landed at exactly 21.0 s (persist delta 20 → 20·21/2=210 ticks) not ~2 s: a **CONFIRMED FOUNDATIONAL executive tick-advance bug, [[nsf370-executive-tick-advance]] / issue #40** — `evt_mainloop` advances `nsftmr_run(1u)` per STIMER wake while `nsftmr_run` re-arms the STIMER for the whole head delta, so a delta-N timer fires after N(N+1)/2 ticks (persist/RTO cadence + the production **2MSL 600 ticks → ~5 h**, since `nsfmain.c` uses the same loop; `nsfmain.c:289` already flagged "nsftmr_run(1) tick accounting correct once real timers exist"). **Separate blocking prerequisite for Mike to rule on (do NOT fold the foundational timer fix into this TCP PR — Kitchen-Sink/rollback risk).** The persist/RTO **policy + backoff are host-proven** (deterministic `nsftmr_run`, 1954 assertions); only the live cadence follows #40. **M4-4 host + over-send fix DONE + committed (PR #39); live persist "backoff-visible" acceptance blocked on #40. M4-5 (EZASOKET M4 verb set — SELECT/FIONBIO/GETPEERNAME + TCP reachable in the NSF module) next.** **Issue #40 FIXED (ADR-0034) — the executive timer arming/consumption contract (host + cross-link + alias scan DONE; live re-validation gates GREEN, PR #41 pending countersign).** Foundation fix in NSFTMR/NSFEVT, no TCP/driver/API change. The loop now advances the timer queue by the ARMED tick count via **`nsftmr_wake()`** (not `nsftmr_run(1u)` per wake), so a delta-N timer fires after N ticks (was N(N+1)/2 — a delta-20 persist probe at 21 s, the production 2MSL at ~5 h). NSFTMR owns the `nsfstim.h` seam through one word `g_armed` = the armed interval, so the invariant **queue empty ⟺ STIMER disarmed ⟺ `g_armed == 0`** holds, enforced at BOTH drain points (`nsftmr_run` on a fire, **`tmr_cancel` on a cancel** — the gap that let the self-re-arming exit fire forever on an empty queue, an advisor-found blocker). `tmr_start` arms whenever the inserted timer becomes the head (empty→nonempty bootstrap + head-shortening), re-arming from "now" — safe-late for queued timers, NO `TTIMER`-residual seam change (the audit: v1's 2MSL tolerates the sub-interval lateness, the urgent rexmit/persist fire on time). `nsftmr_run` + the delta-queue local rules UNCHANGED → M4-4 TCP tests pass verbatim (the red-line). New external `nsftmr_wake` (`NSFTMWAK`) + NSF_DEBUG probe `nsfevt_tickadv` (`NSFEVTKA`); `tmr_arm`/`tmr_disarm` static/inlined. The `nsfmain` `nsftmr_plat_arm(1u)` heartbeat is KEPT as the idle-liveness wake only (bootstrap role gone; removal gated on a proper idle-MODIFY test — TSTEVTM is a vacuous judge, it never issues an idle MODIFY). Host **1954→1985** (TSTTMR 43→71 cadence vectors incl. the delta-20 #40 regression + bootstrap-arm + cancel-disarm probes; TSTEVT 17→20 loop advances armed=K); **ASan+UBSan clean**; NSF+NSFECHO+38 test modules cross-link clean; alias scan clean. New live gate `test/mvs/tsttmcad.c` (host=false): a delta-20 timer through the full loop path fires at ~2.0/4.0/6.0 s (was 21 s). Spec v1.33 (§6.3 + changelog) + ADR-0034 + ADR-0011/0023/0033 annotations. **VALIDATED LIVE on MVSCE (PR #41):** TSTTMCAD fire at **2.000/4.000/6.001 s** (was 21 s); TSTTCPR persist probes at **2/4/8/16 s** exp-backoff on the wire (non-vacuous conns=1/wndprobe=5/rexmit=0); TSTTMACC mean 100.2 ms jitter 0; TSTEVTM 10/10; TSTTCPD rexmit=0 leak-clean; ping **1000/1000** 0 % (NSFIP in/out 1000, NSFICM outecho 1000, drops 0); NSFECHO sizes/kill9/quit + paced echo 200/200 (rapid-fire 1000-echo is a UDP-throughput ceiling on the MIH-degraded pair, provably independent of the fix — g_armed==0 during UDP echo so `nsftmr_wake`≡`nsftmr_run(1u)`). [[nsf370-executive-tick-advance]]. **M4-5 done (host + cross-link + alias scan; live gate `tstezat.c` + the `telnet` `NSFTECHO` gate pending countersign) — the EZASOKET M4 verb set + SELECT (ADR-0035).** NSFEZA gains the stream/control C API (`nsf_connect`/`listen`/`accept`/`send`/`recv`/`shutdown`/`getpeername`/`select`/`setsockopt`/`getsockopt`/`fcntl`/`ioctl`, unique `@@NS*` aliases), the EZASOH03 decoder + `maclib/nsfezasm.mac` grow to match (new codes **SOPT/GOPT**; the SET/GETSOCKOPT plist has no LEVEL — IBM encodes it in OPTNAME — so the facade path defaults SOL_SOCKET, the C API is level-aware). **SELECT is one request over N sockets** → its own engine **NSFSEL** (`src/nsfsel.c`/`include/nsfsel.h`): a fixed static pool of **4** parked SELECTs, each with an EMBEDDED timeout TMR, reached ONLY through registration seams (`soc_set_select_notify` in NSFSOC, `nsfreq_register_select` in NSFREQ) so a build that omits `nsfsel.c` is byte-for-byte unchanged (`RQ_SELECT` stays EOPNOTSUPP, the poke is a NULL no-op). Readiness is a side-effect-free **`PROTOPS.poll`** probe — a precise `tcp_poll` (rxq/acceptq→READ, RCVFIN→READ, ESTABLISHED+room→WRITE), a NULL-poll generic fallback for UDP/dummies (read=rxq/acceptq non-empty, write=always) — and a readiness change pokes **`soc_notify_ready` at the queue/state edge, NEVER gated on a `pend_*` slot** (that slot is NULL exactly when a SELECT is the waiter — the load-bearing rule; the read poke sits at the END of `tcp_recv_data`, post-drain, so a concurrent parked RECV that consumed the segment leaves the SELECT correctly not-ready; the write poke in `tcp_process_ack` after SND.UNA advances, not behind `tcp_send_resume`'s pend_send early-return). **Teardown-while-parked → `ECONNABORTED`** (not "mark-ready"): `soc_destroy` pokes **`SEL_DEAD`** at the top of the ONE checklist (before the gen bump, so `soc_desc` still resolves) — forget-proof, and it is also the non-blocking-CONNECT-failure path (a refused connect → `tcp_close_done`→`soc_destroy`→SEL_DEAD aborts a parked SELECT-for-write). Masks are socket NUMBERS, right-to-left fullword bits → translated to internal descriptors in the FACADE and carried as an `NSFSELITEM` array via the frozen NSFRQE's `ubuf` (Phase-1 same-space; a keyed move in Phase 2, ADR-noted) — **NSFRQE layout unchanged**, one new provisional errno `EINPROGRESS 36`. **Non-blocking CONNECT** completes -1/EINPROGRESS and proceeds (a small `tcp_connect` branch; SELECT-for-write reports completion). **FIONBIO** is a FACADE attribute — a per-app non-blocking bit next to the descriptor (SOCKCB stays spec-exact, no flags field); F_GETFL/F_SETFL(O_NONBLOCK) + IOCTL(FIONBIO) flip it, and every subsequent request carries `RQ_F_NONBLOCK`. **SHUTDOWN** = a trailing `PROTOPS.shutdown` op: HOW 1/2 send FIN (WITHOUT owning the socket's death — the app keeps reading), HOW 0/2 mark local EOF. **SETSOCKOPT/GETSOCKOPT** minimal set (dispatcher-handled): GET SO_RCVBUF/SO_SNDBUF→4096, SET SO_REUSEADDR/TCP_NODELAY accepted (no v1 effect), else EOPNOTSUPP + option in the trace. **TCP + UDP join the `NSF` load module** (spec-D plan of record): a SYN to a closed port now draws a correct RST (`nsftcp_input`), a UDP datagram to an unbound port a port-unreachable — instead of the IP layer's blanket ICMP protocol-unreachable (which also removes the host-kernel ENOPROTOOPT caching from the live-run friction); the request/SELECT path is inert until an app drives it, `S NSF` stays clean. `NSFTECHO` (`samples/nsftecho.c`, `jcl/NSFTECHO.jcl`, README) is the M4 exit-gate TCP echo sample (listen/accept/echo, sequential connections, QUIT sentinel raw-byte). **THE POKE PLACEMENT RULE and the ECONNABORTED teardown were advisor corrections**; the advisor also flagged that `tcp_poll` must be tested DIRECTLY (a dummy-poll engine test proves only that the engine calls poll) — done in TSTTCP via the exported vtable. Host **1985→2152** (TSTSEL 47 — engine park/poke/timeout/EMFILE/teardown/pollfallback direct-call; TSTTCP +12 `tcp_poll` each source + non-blocking-connect success/refused; TSTEZA 64→160 — the M4 verbs through the core, accept→new-number, EBADF/ENOTCONN matrix, FIONBIO persistence, sockopt min-set, EZASOH03 M4 codes incl. the **SELE decode** (copy-send→ret mask, run-in-place, ERETMSK zeroed), and the **SELECT mask byte-exactness for descriptors 0/31/32** made-ready-before so it never parks in the threaded harness); **ASan+UBSan clean** on tsteza/tstsel/tsttcp (the mask-writing off-by-one ground); NSF+NSFECHO+**NSFTECHO**+40 test modules cross-link clean (cc370/as370/ld370), alias scan clean (new `NSFSL*`/`NSFSOSSN`/`NSFSONTR`/`NSFRQRSL`; 23 `@@NS*` unique ≤8 chars). NSF module 284 KB (with TCP/UDP/socket/select). Live gate `test/mvs/tstezat.c` (host=false, deterministic LISTEN/FIONBIO-accept-EWOULDBLOCK/SELECT-2s-timeout-RETCODE-0 as hard CHECKs; accept-echo + non-blocking-connect host-coordinated + bounded) + the `telnet`-driven `NSFTECHO` — **pending Mike's countersign**. Spec v1.34 (§15.2 + changelog) + ADR-0035 + conformance §2.2/§3.1-3.2. **VALIDATED LIVE on MVSCE** (real 0500/0501): **`S NSF` clean startup** with the changed `nsfmain.c` (NSF000I→NSF210I/211I→NSF001I, no dump — the regression risk cleared); **ping 1000/1000 0 %** (receive path unaffected by TCP/UDP/socket/select in the module); **closed-port RST byte-exact on the wire** (`[R.] seq 0, ack SEG.SEQ+1` — the new behaviour, was ICMP proto-unreach); `F NSF,STATS` shows the new NSFSOC/NSFREQ/NSFTCP counters; `P NSF` clean. **TSTEZAT batch 20/21** — all M4 verbs live (LISTEN, FIONBIO→EWOULDBLOCK, SELECT-event→accept→getpeername→`RECV n=14`, non-blocking-connect EINPROGRESS→completed, termapi, leak gate); the 1 FAIL was a test-race (the host connected in the "idle" SELECT-timeout window; SELECT correctly returned rc=1) — fixed by selecting on a fresh idle socket. **NSFTECHO telnet echo — the M4 exit gate — GREEN**: on the wire 3WHS + byte-exact echo + clean 4-way FIN + sequential accept (2 connections) + `QUIT→BYE`, then **clean shutdown CC 0, leak gate clean** (`conns=2 echoed=11 quit=yes`, `established=2`). **A REAL foundational bug fell out of the live NSFTECHO shutdown (my "degraded pair" attribution was WRONG — it reproduced on a fresh pair):** the executive loop hung after `nsfevt_stop()` from the app subtask. Root cause — `nsfevt_stop` posted the stopECB via `nsfevt_plat_post` (BIT-ONLY on MVS: does NOT wake a task already committed to the SVC-1 WAIT); NSFECHO (UDP, no timers) never hit it because its STIMER **heartbeat** kept the loop spinning, but NSFTECHO's **TCP timers drain-disarm the STIMER** at teardown (`tmr_cancel`→`tmr_disarm`, ADR-0034), removing the heartbeat → the bit-only stop never woke the WAIT → deadlock (exactly ADR-0034's flagged "does the loop wake on an idle event without the heartbeat" empirical question, now answered NO). **Fix: a new `nsfevt_plat_wake` seam** — a real SVC-2 POST (`ecb_post`, no cthread identity required) on MVS, the cond-broadcast on host — used by `nsfevt_stop` for the standalone cross-task stop signal (the bit-only `nsfevt_plat_post` stays for posts always accompanied by a real device/request wake). Same lesson as `soc_complete` (real POST, not the bit seam). Host **2152** unchanged (the host seam already cond-broadcasts); NSF+NSFECHO+NSFTECHO+40 test modules cross-link clean (`ecb_post` autocall); NSFTECHO shutdown DBG-marker trace pinned it to `evt_mainloop` not returning. **Diagnostic lesson:** a hung MVS job's SYSPRINT is buffered + lost on the S222 cancel — use **WTO (`nsfmsg`) markers** (console log survives the hang), not printf, to locate a teardown hang. New external `nsfevt_plat_wake` (`NSFEVPW`). [[nsf370-executive-tick-advance]] [[nsf370-nsfevt-stop-real-post]] **M4-6 done (host + cross-link + alias scan + live gate GREEN) — the loss-injection harness + TIME_WAIT reclaim; the M4 exit gate.** `test/tstloss.c` (host-only, `mvs=false`) drives TWO real NSF TCP sockets self-talking on ONE stack (same HOME_IP, the 4-tuple demux distinguishes them — the port pairs are swapped) through a lossy loopback: a **synchronous single-threaded pump** (no event loop, no threads — the M4 test contract) + a capture DEVOPS byte-FIFO + a seeded-PRNG fault stage (drop/dup/reorder-hold-one + deterministic first-K SYN/FIN drop; the seed is printed so any failure reproduces). The pump drains a per-round SNAPSHOT (frames captured before the round; injection appends to the next round — what stops it spinning as ACKs regenerate), flushes a reorder-held frame on a stall, else jumps sim-time to the next armed expiry (`nsftmr_peek(0)->delta`), else declares a real deadlock — **it never nudges past a stall** (the kickoff's stop-and-report line); a livelock watchdog FAILs fast with the seed on any no-app-progress spin. Scenarios (each a seeded transfer verified BYTE-EXACT + orderly FIN + per-scenario leak gate): 5 % drop / mid-transfer 3-seg burst / 5 % dup / 5 % reorder / combined 5 %+2 %+2 % (3 seeds @ 1 MB + 20 rotating @ 128 KB) / SYN+FIN loss; **TSTLOSS 511/511**, all 1 MB byte-exact, ~3 s. **The harness exposed THREE real, latent TCP bugs — all folded in (Mike's standing "fold in + auto-fold further"), each host-regression-guarded:** (1) **active-open window** — `tcp_synsent_input` adopted the SYN|ACK window via the CONDITIONAL `tcp_update_window` with `snd_wl1==0`; `TCP_SEQ_LT(0, upper-half-seq)` wraps FALSE for a peer ISS ≥ 2³¹ (~half of all peers, incl. NSF's own passive side) → `snd_wnd` stuck 0 → the active opener NEVER sends. Fix: set snd_wnd/wl1/wl2 unconditionally on the synchronizing segment (the passive child already did; RFC 793 initial-window set is unconditional). Masked because every prior test used a lower-half synthetic peer ISS and live active-open never sent guest data. Guard `test_synack_window_high_iss`. (2) **persist-probe ACK** — the zero-window probe sends 1 byte at SND.NXT without advancing it (ADR-0033); when the peer's window REOPENED it accepts the byte + ACKs SND.NXT+1, which `tcp_process_ack` rejected as "acks unsent" — dropping the window update → **livelock** when the window-update ACK was LOST (only the loss path produces this). Fix: accept a probe-ACK (`ack==snd_nxt+1`, persist armed, unsent data) → advance snd_nxt, resume. Guard `test_persist_probe_ack_reopens`; ADR-0033 §4 annotation. (3) **active-path TIME_WAIT reclaim** — `tcp_attach` didn't reclaim (only the passive `tcp_child_create` did) → a guest doing rapid active connect→close vs a REMOTE peer walls at EMFILE once its own TIME_WAITs fill the pool. Fix: tcp_attach reclaims on exhaustion. Guard `test_timewait_reclaim_active` (self-talk masks it — the local passive path covers, so a synthetic remote peer is needed). All three are intra-function (**no new externals**). **TIME_WAIT reclaim under pool pressure** demonstrated host-side (`test/tstloss.c` `run_reclaim_test`: 52 active connect→close cycles, pool 40, twreclaim delta 22, all succeed, pools baseline) and live (`test/mvs/tsttcpw.c` + `samples/host/twreclaim_listener.py`: the guest loops 40 active connect→close against a host listener that lets the client close first — so the guest is the active closer → guest TIME_WAIT — with a 32-TCB pool forcing reclaim; the gate: all 40 succeed + twreclaim>0). Full host suite **2788/0** (26 tests), ASan+UBSan+Werror clean on TSTLOSS+TSTTCP, cross-build NSF+NSFECHO+NSFTECHO+**41** test modules clean, alias scan clean. Spec v1.35 (§13 status + M4 exit gate + changelog) + ADR-0033 annotation + conformance UNCHANGED (no errno surface change). **VALIDATED LIVE on MVSCE** (real 0500/0501, `test/mvs/tsttcpw.c` + `samples/host/twreclaim_listener.py`): TSTTCPW **batch CC 0** — `cycles_run=40 connected=40 sock_fail=-1`, **`twreclaim before=0 after=8`** (delta 8 = cycles 40 − pool 32, exact), `estab=40 rstrcvd=0 rexmit=0`, no EMFILE wall, leak gate clean; `tcpdump -ni tun0 tcp port 3002` shows every cycle 3WHS + the **guest (.1) sending the FIRST FIN** (the active closer → guest TIME_WAIT) over distinct ephemeral ports (49152, 49153, … 49191), the listener served all 40. The TSO re-run FAILs by design (the one-shot `--count 40` listener was consumed by the batch run → every re-connect gets a RST, `rstrcvd=40 errno=61`; batch is the gate). Fixes #1/#2 are host-proven only (not live-inducible without loss injection/root); fix #3 is proven by this live reclaim. **M4 exit gate MET — maintainer-countersigned (PR #43 merged). M4 COMPLETE. M5 (Phase 2: NSFS subsystem + cross-memory + TCP hardening) next.** [[nsf370-m4-6-status]] |
| **M5** | Phase 2: `NSFS` subsystem + cross-memory + TCP hardening + docs | 2 address spaces share one stack; stress passes; docs complete | 🔄 **In progress — the Stage-0 transport probes** (they execute what Notion tracks as *M5-1*, the Phase-2 design review; each is an ISOLATED probe — no NSFRQE, no socket, no protocol code). **Stage-0a done then RETIRED** (SSI transport, ADR-0036, PR #45; retired PR #47) — superseded by **Stage-0a′, the private-SVC transport** (**ADR-0038**, PR #46), **live-green on MVSCE**: an **unauthorized** client (`TESTAUTH` FCTN=1 == 0) crosses address spaces 106× through a **stolen `SVC 239`** (Type 3, **no APF library needed** — [[nsf370-apf-self-auth-svc244]]), pure-HLASM SVC routine, anchor via a patched word, modal-EP scan for the free slot, slot stolen **and restored** at stop, no abend. **Stage-0b done** (**ADR-0039**, PR #48 — the maintainer's merge IS the countersign, per the M3-5/M4-6 convention): the `ubuf` **keyed CSA bounce** — app→CSA staging→STC→app byte-exact via **`MVCK`**, *not* cross-memory (`MVCP`/`MVCS` are DAS and absent on this target), chunk 2048, staging embedded in the anchor (`NSFV_ANCHOR` 2176 B). Live: `TSTMVCK` 16/16 + `TSTUBUF` 21/21 CC 0, sizes 0/1/100/2048/5000/10 all byte-exact with the **guard byte after `ulen` untouched**, `SVC 239` restored, no dump. **Toolchain finding: `as370` mis-assembles the `MVCK` mnemonic** (drops the `R1` length and `R3` key registers) → emit `MVCK` as a **raw `D9` opcode** in **≤255-byte pieces**; the `as370 -a=` listing is the gate, not a green link. **Stage-0c done — the LAST Stage-0 gate, live-green** (**ADR-0040**): the **client-death guard**. Client liveness is an **ASVT lookup run immediately before every reply POST** — not a resource manager (3.8j has no dynamic RESMGR; EOM routines are nucleus/SYSGEN, i.e. a USERMOD + IPL) and not a periodic reaper (a sweep leaves a window in which the STC still posts into an address space that died since the last run). The SVC routine now captures the caller's **ASID** (`ASCBASID`, ASCB+`X'24'`) next to its ASCB, and the STC classifies that pair: `asvtenty[asid-1]` **available bit → DEAD**, **ASCB-address mismatch → DEAD** (the ASID-reuse row an ASID-only check cannot see), **equal → LIVE**. DEAD is **reaped** (in-flight count back, staging cleared, slot released) and **never posted into**; **UNKNOWN** (no ASCB / no ASVT / ASID out of range) is **neither** — a new `HELD` state, since the executive drains while `PENDING` and would otherwise spin. **Compare the ASCB ADDRESS only** — a reused SQA block holds a stranger's fields. **The safe-side asymmetry is the rule everything bends around:** a dead client called live leaks a slot; a live client called dead has its storage freed underneath it. **ufsd #53's own-ASCB row deliberately does NOT transfer** (there the checker is the server's own restart; here it is the server checking a *client*, where the same compare would kill a client running inside the STC's AS). Live on MVSCE (**full Stage-0 regression at the new layout: `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH` all CC 0 batch+TSO, 444 PASS**, unauthorized client): both DEAD rows reaped with `NSFV050I` (`inflight`→0), UNKNOWN `HELD` with `NSFV051W`, **no false positive on either LIVE path** (which is also what proves the `asid-1` index), `SERVED=4 INFLIGHT=0 REAPED=4`, `SVC 239` restored, CC 0000, no dump. `NSFV_ANCHOR` 2176→**2184**, `NSFV_REQ` 28→**48**; every field now carries an `NSFV_OFF_ASSERT` (a size assert cannot catch a field that *moved*). **Synthetic by construction:** a living client stages a dead *identity* (probe-only `ORPHAN`/`QUERY`/`UNSTAGE` verbs) — the kill-vs-POST race is a documented manual check. **A live-only regression fell out of re-running 0b at the moved layout** (`stage` +128→+136): inserting the ORPHAN staging block between the XFER block and the shared POST block broke a **fall-through**, so every XFER re-staged the ORPHAN identity (zeros) → guard UNKNOWN → the client hung. Fixed with an explicit `B DOPOST` on every staging block. **New shape of the old rule: a clean link, passing offset asserts and a correct-looking `as370` listing cannot see that control reaches the wrong block — an asm change under a moved layout is validated by re-running EVERY stage's live gate, not just the new one.** **Stage-0c is COUNTERSIGNED (review on PR #50) — the last Stage-0 gate is green and M5-2 is UNBLOCKED.** M5-2 inherits **six** documented obligations, two from 0b and four from 0c. From 0b: **the write-out key window** (the read-out still stores into `ubuf` under PSW key 0 — the keyed protection is half-closed) and **the moved-length contract** (the `min(ulen,2048)` clamp is silent and the chunk size is shared by convention; BSD `send`/`recv` return the moved count, so M5-2 must too, or a drifting client sees silent truncation), From 0c: **the `owner_ascb` sweep** for sockets that outlive their request — which must reuse **this** classifier, not `ufsd_sess_cleanup`'s ASID-only test; **remove the probe scaffolding** (`NSFV_REQ_ORPHAN` + the `pascb`/`pasid` words — a request-supplied identity is precisely what the guard must never trust for a real client, and it must not be mistaken for a transport path); **extract the classifier arithmetic and host-test it** the way ufsd did into `ufsd#asv.c` (the maintainer answered 0c's open question: yes, in M5-2 — the range check, the AVAIL bit, the address compare and all four verdicts are pure arithmetic, so a host test pins the truth table instead of leaning on a live run to hit every row); and **decide whether UNKNOWN at *shutdown* differs from UNKNOWN at *service* time** (the drain's nudge is gated on LIVE, so a client parked on an UNKNOWN request is never nudged and the STC retains CSA). **M5-2a (the NSFRQE crosses for real) — DONE, maintainer-countersigned (PR #52
merged).** M5 itself stays **in progress**: M5-2a is the first of five sub-steps (a–e). ADR-0041: the frozen 64-byte NSFRQE is **copied into a CSA request
slot** and dispatched from an **STC-private key-8 copy**, never from the CSA block —
so `soc_complete`'s SVC 2 POST never targets key-0 storage from a problem-state key-8
task (an open 3.8j question, in the host-clean/link-clean/live-wrong shape), and
**`nsfsoc.c` is untouched**: no socket/protocol/TCP/UDP file learns a boundary exists,
satisfied *structurally* rather than by care. `nsfreqx_dispatch_in` rewrites exactly two
fields — `ubuf`→the staging buffer (a caller-AS address reads the wrong space from the
STC) and `ulen`→the count **actually staged** — which discharges ADR-0039's
moved-length obligation through the **already-frozen `retcode`**: no new NSFRQE field,
**freeze intact**. Completion is an **end-of-pass POSTED-bit check on the private ECB**
(parked ops complete long after dispatch returns), and "completed but not yet replied"
joins the WAIT-gate probe (ADR-0025 defect (2) / #27 class). The STC's wake ECB lives in
**STC-private key-8 storage** (`server_ecb_ptr`, published BEFORE the SVC slot is stolen,
invalidated after it is restored): the executive WAITs from problem state, where a key-0
CSA ECB in the ECBLIST is a **documented abend** (S047 / X'201', ufsd cross-AS reference);
zero means "not published" and the routine falls back to `server_ecb`, which keeps the
probe STC byte-for-byte. Anchor 2184→**2256** (`rqe`@2184, **`rqe_guard`@2248**, `server_ecb_ptr`@2252). The
**guard word** sits between the RQE slot and the published wake-ECB address because they
would otherwise be byte-adjacent: a one-byte overrun on the 64-byte RQE move would write
the pointer's high byte, and that does NOT fail cleanly — the routine's `LTR`/`BNZ` still
sees non-zero, takes the key-8 branch and issues a **key-0 cross-AS POST to a corrupted
address in the STC's private storage**, no abend guaranteed (the Stage-0b guard-byte
precedent, applied to the field that can least afford to be wrong). It is a **non-zero**
4-character eyecatcher (so a zeroed anchor is distinguishable from a clobbered guard) and
the detection is **two C comparisons in the STC's hop-2 path** — the guard, and
`server_ecb_ptr` against the STC's own `&g_wake_ecb` — each reaping through the existing
0c path and **never POSTing**; the asm gained **two EQU values and no instruction**. The
truth table is host-pinned in TSTREQX. Fields are **appended** where they can be; moving
`server_ecb_ptr` +2248→+2252 finishes an **unreleased** layout (neither field exists on
`main`), which is not the case the "never insert" rule guards against; `NSFV_REQ` 48→52
(`rqeimg`). New modules: **NSFS** (`src/nsfsx.c` + `src/nsfsmain.c`, `ac=1`, generated
from `nsfmain.c` — three differences) and the client seam `nsfreq_set_transport` +
`src/nsfreqc.c`, **inert until registered**, so the 20 modules linking `nsfreq.c` and the
**C/EZASOKET/EZASMI surfaces are unchanged — apps relink only**. `src/nsfmain.c` and the
`NSF` module are untouched. **VALIDATED LIVE on MVSCE** (`tun0` up, `hercifc` setuid,
CUU 0500/0501 ONLINE, deployed LINK **MTU 1500** read from `SYS2.PARMLIB(NSFPRM0)` and
confirmed by `NSF210I ... MTU 1500`): **TSTRQXM batch CC 0, 26/26** — unauthorised client
(`TESTAUTH == 0`), INITAPI/SOCKET/BIND/GETSOCKNAME/CLOSE/TERMAPI across the boundary, the
bound port back through the result fields, EBADF (dispatcher) **and EMSGSIZE (a protocol
op)** back with the specific errno; **obligation #2 PROVEN** — TCP `send` of 5000 reports
**2048, never 5000**, chunk→2048 and chunk+1→2048 at the boundaries, the short-write loop
drains 9096 bytes and the host peer verifies **every byte exact**
(`samples/host/shortwrite_listener.py`). The TCP `connect`/`send` are also the **first
live exercise of the parked-request path** (§5's completion check + the un-posted-private-
ECB fix, `ed4d209`) — synchronous verbs never park, so nothing before this tested it.
Stage-0 regression at the current layout: **TSTSVC/TSTMVCK/TSTUBUF/TSTDEATH all CC 0
batch+TSO, 444 PASS** — but those prove the **CSA fallback** branch, NOT the key-8 POST
(the probe publishes no pointer); the key-8 branch is proven by TSTRQXM alone. NSFS and
NSFV each start/stop clean, **SVC 239 stolen and RESTORED**, `IEF404I`, no abend, no dump.
Host **2788→2846** (TSTREQX 58; a **real bug** fixed en route — the private copy inherited
the caller's `ecb` word, whose POSTED bit would make the STC reply with an untouched
`retcode`; the old tests dodged it because `0x11223344 & 0x40000000 == 0`; new assertions
FAIL against the unfixed code). **Live-unproven and deferred:** the write-out key window
(**b**), concurrency / the MAXSOC slot pool (**b**), the `owner_ascb` sweep + probe-
scaffolding removal (**c**), validation + UNKNOWN-at-shutdown + the guard-arithmetic host
test (**d**), 2-AS stress (**e**); the mirrored **STC-death race** is recorded as residual
risk in ADR-0041, not closed. The TSO re-run of TSTRQXM FAILs by design (one-shot host
listener consumed by the batch run → `errno 61`; batch is the gate — the TSTTCPW
precedent). [[nsf370-m5-2a-status]]
**M5-2b0 (the destination-key probe) and M5-2b1 (the write-out key window) — DONE,
live-green; b1 maintainer-countersigned (PR #58).** M5-2b is the second of the five sub-steps; **M5 stays
in progress and no milestone flips**. **b0** (`test/mvs/tstmvcd.c`, ADR-0039 annotation,
TSTMVCD **58 PASS CC 0 batch+TSO**) answered three questions and fixed nothing: a
**destination-keyed move does NOT exist** here — `MVCDK`/`MVCSK` both take an **operation
exception (S0C1)** in supervisor state, the prediction recorded in the source before the
run, corroborated from primary source first (Hercules `opcode.c` gates both
`GENx___x390x900`, so the S/370 slot is `operation_exception`; `ARCHMODE S/370`). **Two
corrections to the briefed encoding, both from primary source:** the opcodes are **`E50F`/
`E50E`**, not `B20F`/`B20E`, and the registers are **R0 = length MINUS ONE, R1 = key**, not
the reverse. An **`SPKA` window DOES close the hole**: with `IPK` read INSIDE the window
confirming `X'80'` every time, a key-8 store into key-0 storage **faults `S0C4`** on **two
independent destinations** (a `GETMAIN SP=241` block taken in key 0, `ISK` `X'06'`; and an
own frame `SSK`'d to key 0), while a key-8 **fetch** of that CSA block **succeeds
byte-exact** because it is not fetch-protected (bit `& X'08'` clear) — so under the
caller's key both halves of the move are reachable, no landing area needed. **CSA budget:
2064 KB total** (correction: the GDA has **no** CSA size field — `CVT+X'230'` → `GDA+8
CSAPQEP` → `PQE+20 PQESIZE`/`+24 PQEREGN`), largest `SP=241` `GETMAIN` **≥ 1 MB** (a floor:
the doubling search is capped at 1 MB and never failed). **b1** (ADR-0039 + ADR-0041
annotations) closes the window on **both** M5-2a destinations, `ubuf` **and** `rqeimg`:
`RQEOUT`'s two moves run inside a **narrow per-piece `SPKA` window keyed from `TCBPKF`**
(TCB+`X'1C'`, high nibble, verified against `SYS1.AMODGEN(IKJTCB)`: `TCBFLAG EQU X'F0'` /
`TCBZERO EQU X'0F'` — which is why libc370 goes straight `IC`→`SPKA` with no shifting),
the TCB reached by **`PSATOLD`** and NOT `R4` (the ancestor documents `R4 = A(TCB)` at SVC
entry, but `R4` is loop scratch in `RQEIN` and the POST save/restore preserves the
*clobbered* value). The move is a **plain `MVC` reached by `EX`** — b0 proved a keyed move
is unnecessary under the caller's key, and `MVCK` inside a non-zero PSW key would put its
`R3` through the CR3 key-mask check that already cost `tstmvck.c` an `S0C2`. **`EX`
supplies LENGTH-1** (the piece cap moves 255→**256**; the 255 was an `MVCK` ambiguity guard
and stays in `RQEIN`), and it ORs into a **copy**, so the routine stays RENT. `IPK` writes
`R2` — the anchor base — so `R2` is parked across it, once, not per piece. **`RQEIN` is
untouched** (its destination is key-0 CSA and its source is already keyed via `MVCK` `R3`),
and **`XFEROUT` deliberately keeps its key-0 store** as Stage-0b probe scaffolding.
**Gates:** the `as370 -a=` listing is the gate and was checked — `SPKA 0(R9)`→`B20A 9000`,
`SPKA 0(R12)`→`B20A C000`, `EX R1,MVCPIEC`→`4410 6362` (base `R6`, not dropped to base 0),
target `MVC 0(1,R4),0(R5)`→`D200 4000 5000` (length byte `X'00'`, so the OR yields exactly
L-1). **TSTRQXM batch CC 0, 32/32** with the host peer verifying **9353 bytes byte-exact**,
including the new **`EX` boundary**: 1-byte and 256-byte sends each byte-exact in the
direction the window protects, plus a whole-buffer sweep for overrun (the multi-piece path
needs no duplicate — a 2048 chunk is eight 256-byte pieces). Stage-0 regression green:
**TSTSVC/TSTMVCK/TSTUBUF/TSTDEATH all CC 0 batch+TSO**; NSFS and NSFV start/stop clean,
`SVC 239` restored, **no dump**; host **2846 PASS / 0 FAIL**. **The fault-path assessment
(b1 assesses, it does not fix — recovery stays the open M5-2 item ADR-0039 names):**
`test/mvs/tstrqxf.c` induces the fault live — **`S0C4`, caught, no dump, client alive,
transport not wedged** — and reads the anchor back: **`req_state` FREE, `inflight` leaked
at 1**, as predicted. **That measures the IN direction, not the out:** `RQEIN` reads `ubuf`
before `RQEOUT` writes it and both use the same pointer, so any pointer bad enough to fault
the write-out faults the read-in first — the **pre-existing** exposure. The one class that
gets past the read-in is **key-0 non-fetch-protected storage** (`MVCK` permits a key-8
read; a key-8 store is denied) — exactly the old silent clobber, and exactly what cannot be
handed to a test safely. The out direction is therefore **reasoned from a branchless code
path**: `req_state` stuck at **DONE** (slot busy forever, later requests `RCNOREQ` — *worse*
than the in-direction), `inflight` leaked, staging retained; `UNSTAGE` *can* recover that
one. **The PSW key does not leak usefully** — `SPKA` back never runs on the fault path, but
the borrowed key is the caller's own and RTM resumes the caller's RB with the caller's PSW;
measured, since the client went on to QUERY, UNSTAGE and run stdio normally. **Two adjacent
findings, reported not fixed:** `UNSTAGE` early-returns on a FREE slot so it **cannot
recover a pre-publication leak**, and **`nsfsx_stop()` does not drain `inflight` at all**
(it frees the CSA unconditionally — `P NSFS` was same-second with `inflight` = 2), unlike
the probe STC's `nsfv_drain` (10 s ceiling, retain-CSA on timeout). **b1 does not worsen
the exposure:** the fault class pre-existed; b1 turns a silent key-0 clobber into a caught
fault and leaves the dangling-state shapes unchanged. Anchor layout unmoved, **NSFRQE still
frozen at 64 B**, C/EZASOKET/EZASMI surfaces unchanged — apps relink only.
**M5-2b2 (the POST save area moves to the SVRB) — DONE, live-green,
maintainer-countersigned (PR #60).**
Closes ADR-0038's RENT/shared-scratch caveat: `csasave` was ONE 18-word block in the CSA
anchor that every invocation in every address space computed the same address for and stored
into. It is now the **SVRB's own `RBEXSAVE`**, which the FLIH allocates per SVC invocation —
no lock, no pool, no `GETMAIN` (the pool is still b3). **Offsets READ, not guessed:**
`SYS1.AMODGEN(IHARB)`/`(IKJTCB)` read live and computed by **IFOX00 from the macros** (jobs
RBOFF/RBOFF2) — `RBEXSAVE` at `RBSECPTR+X'60'`, `L'`=`X'30'`; `RBBASIC-RBPRFX`=`X'40'`;
`RBSIZE-RBBASIC`=8; `TCBRBP-TCB`=0. **Twelve words, not eighteen** — the old
`STM R14,R12,12(R13)` wants 72 bytes and does not fit; only **four** registers are live
across the POST (R2, **R6 the CSECT base**, R8, R14), so four stores of 16 bytes replace an
STM of eleven, deliberately minimal so the question of who owns the rest of the area stays
small. **The bug that cost the first attempt is b1's lesson one register over:** `A(SVRB)`
must come from **`TCBRBP`, not `R5`** — the FLIH does set R5, but **R5 is the `MVCK` source
pointer in `RQEIN`/`XFERIN`**, so at DOPOST it holds A(caller image). Measured
`R5=000991EC` vs `TCBRBP=009DE5F0`, `RBSIZE` off TCBRBP = 28 doublewords = 224 B (sensible)
vs garbage off R5. The first attempt therefore put the save area **inside the client's own
storage** and stored the caller ASCB over the test's variables — the diagnostic read back
`C1E2C3C2`, EBCDIC **"ASCB"** — **while every gate in the set stayed green**. **The
prediction was REFUTED:** a canary in `RBEXSAVE` survives the POST **and** the WAIT (task
switch + cross-AS POST + SVC 1); the "available while running but not across a suspension"
hypothesis did not hold, and the single cause was the clobbered R5. **The positive check is
permanent** — TSTRQXF now asserts the area is outside the anchor **and outside the client's
own storage** (the first attempt's failure mode was neither, and no "outside the anchor"
test would have caught it), the stamp took, the sentinel survived POST and WAIT, and the
**register half** (reaching past the post-WAIT eyecatcher proves restored R2 *and* R6).
`ANCSAVE` stays **dead in place** (removing it shifts every later field; b3 moves the layout
anyway) — **anchor layout unmoved, no `NSFV_OFF_ASSERT` changed**. The ancestor
`igc0024e.asm` saves no registers anywhere and never dereferences R5: it corroborates the
entry convention and says **nothing** about the save area — neither supporting nor
undermining `RBEXSAVE`. A **column-71 overrun was caught by the scan** mid-step, a live
sighting of the §3 rule: a 73-char instruction line made as370 swallow the next instruction,
emitting `ST R14,0(,R9)R2,4(,R9)` — one store gone, clean link, no diagnostic. Gates:
TSTRQXF **68 PASS CC 0 batch+TSO**, TSTRQXM **batch CC 0 32/32** (host peer 9353 bytes
byte-exact), TSTSVC/TSTMVCK/TSTUBUF/TSTDEATH **444 PASS CC 0 batch+TSO**, NSFS+NSFV
start/stop clean, `SVC 239` restored, **no dump**; host **2846 PASS / 0 FAIL**. **No
milestone flip.**
**b1 follow-up — the gate now DISCRIMINATES, and the window's scope is narrower than
"closed".** No production source touched (`asm/nsfvsvc.asm` byte-identical, anchor layout
unmoved, NSFRQE still 64 B). (1) **The discriminating case exists after all.** b1 said the
one class that would discriminate — `ubuf` in **key-0 non-fetch-protected** storage, which
gets PAST the key-8 read-in — could not be handed to a test safely; right about the class,
wrong about "safely". `tstrqxf.c` now points `ubuf` at **the anchor's own staging buffer**
(`&stage[1024]`, 64 B): the only storage at risk is NSF's own scratch, and source+dst are
both inside `stage[]`, so a window that failed to take performs a **byte-identical round
trip** rather than a clobber. Self-validating on three independent facts — the anchor
eyecatcher, a key-8 READ that **succeeds**, a key-8 STORE that **faults** — which is what
makes the observed `S0C4` a KEY fault and not a bad address. **New measured fact:** the
**SVC table is readable from problem state key 8** — an unauthorised client chased
`CVT 0001D048 → SCVT 0001D510 → SVCTABLE 0000FA60 → svcentry[239].svcepa 00A6F208 →
+NSFV_ANCH_OFF → anchor 00A6F688` hop-by-hop under `___try` (the kickoff's claim that
`nsfreqc_init` already does this is **wrong** — it only issues a QUERY; the only SVCTABLE
chase in the tree is `nsfv.c`/`nsfsx.c`, both `__super`'d into key 0 for the STORE).
**The revert proves it discriminates:** window IN → `S0C4`, **24/24 batch + TSO CC 0**;
`SPKA` pair commented out (verified out in the `as370 -a=` listing) → the request
**RETURNS rc=0, the store lands**, **23/24 CC 1**; window RESTORED → `S0C4` again,
**24/24 CC 0**, listing byte-identical to b1's. **Exactly one assertion moves.** The
OUT-direction dangling state is now **measured**, not reasoned: `req_state` stuck at
**DONE**, `inflight` leaked, and **`UNSTAGE` DOES recover it** (published slot), unlike the
in-direction. (2) **Scope correction, in the ADRs:** the write-out key window is closed on
the **RQE path**, NOT at the **SVC boundary** — **`FNXFER` is a reachable verb** driven by
`tstubuf.c`, an **unauthorised** client, and `XFEROUT` still stores a caller-supplied
`ubuf` under PSW key 0. So **removing the probe scaffolding is a SECURITY item, not only
hygiene** (M5-2c obligation amended); deliberately **not** pulled forward, since
TSTMVCK/TSTUBUF/TSTDEATH are the regression for every later step. (3) **`LRA`+`SSK` on a
pageable frame is unreliable** (three real frames for one virtual page across three runs) —
CLAUDE.md §3 caveat added **with the direction stated**: these tests expect a fault, so a
lost key **fails** the assertion — **false negatives only**, a passing run is trustworthy
and Stage-0b's conclusion stands (2.2a's `GETMAIN SP=241` destination uses no `SSK` at all).
(4) **Four issues filed, none fixed here:** `tstmvcd.c` 2.2b flakiness, `tstmvck.c`'s
hardcoded `SSK` restore, **`nsfsx_stop()` never drains `inflight` (marked a b3
PREREQUISITE)**, `UNSTAGE` on a FREE slot. Regression green: TSTRQXM **batch CC 0 32/32**
(host peer **9353 bytes byte-exact**), TSTSVC/TSTMVCK/TSTUBUF/TSTDEATH **444 PASS CC 0
batch+TSO**, TSTRQXF **48 PASS**, NSFS+NSFV start/stop clean, `SVC 239` restored, **no
dump**; host **2846 PASS / 0 FAIL**. **No milestone flip.**
**b1 and b2 reached `main` only via PR #62.** The M5-2b stack was merged bottom-up as
three chained PRs, but #58 and #60 went into their *predecessor branch* instead of
`main`, so `main` briefly carried b0 alone while the two branches sat ahead of it. The
repair merge was content-equivalent to a fast-forward — `git merge-tree` against `main`
reported no conflicts and its result tree was **byte-identical** to the reviewed tree —
so **no live MVS gate was re-run**: nothing moved, and the "re-run every stage's live
gate" rule is about a *moved layout*, not about which branch a commit sits on. The
checkable habit: read `baseRefName` (`gh pr list --json number,baseRefName`) before
merging a stacked PR — a child PR's base still names the parent branch until it is
retargeted.
**M5-2b3 (the 64-slot pool) — DONE, live-green except two named gaps;
maintainer-countersigned (PR #63).** The structural step of M5-2b, and where the
transport stops being safe by construction. The
single request area — which turned a second caller away with `RCNOREQ` — becomes **64
independent slots**, each claimed by `CS` on its **own** state word (**ADR-0042**). ABA-free by
construction because *the location being compared IS the resource*: no head pointer can go
stale, which is why there is **no free list** — `asm/nsfxq.asm`'s own header forbids the
per-element pop a free list needs, and it can afford that rule only because it has a single
consumer draining whole chains. **Three writers, three rules:** claim by `CS`, release by plain
`ST` (the owner races with nobody), **reap by `CS` — the reaper is a THIRD observer**. A
correction found while implementing and folded back into the ADR: `CS(observed→FREE)` is still
wrong, because reclaiming means *clearing* CSA, so the reap is **two moves** — `CS` to
`CLAIMED` (which nobody else can claim), then clear, then `ST FREE`; `CLAIMED` therefore means
"owned, not available", not "owned by a client". **Layout:** 48-byte header (`ANC*` EQUs) +
`NSFV_SLOT` 2144 B (`SL*` EQUs) × 64 = **137,264 B**, one contiguous `SP=241` `GETMAIN`, 6.5 %
of the measured 2064 KB CSA. Header is a multiple of 8 so every slot — hence every `CS` claim
word — is doubleword aligned. **Not padded to a power of two:** the scan walks a *pointer*
(`LA Rn,SLOTLEN(,Rn)`) and performs **no index multiply anywhere**, so 2560/4096 would cost
26–120 KB for nothing. `stage[]` stays last, and the consequence is **stated, not fixed**: an
over-long move now lands on the NEXT slot's claim word instead of adjacent CSA — a different
shape of hazard, not a smaller one — and the `min(ulen,2048)` clamp is still the only thing
preventing it, unchanged from b1. **Two cheap defences against a stale `NSFVSVC` in CSA against
a newer `NSFS`** (separate load modules; a mid-chain deploy failure silently keeps the previous
one, §5): the scan's bound comes from the anchor's **`nslots`**, written by the party that
allocated the storage, and the routine **checks `version`**, bumped 1→2 in the same change that
moved the layout. **`csasave` is gone and b2's five-word self-check with it (#61)** — one-time
evidence, collected, recorded in ADR-0038; five stores and three `CLC`s per request to re-prove
a settled fact is not a hot-path trade. **`nsfsx_stop` now drains before it frees (#55)**, which
stopped being inert exactly here: with service serialised, 63 clients are legitimately parked in
ORDINARY operation, and it nudges **every** claimed slot (the probe's single-slot wake does not
survive a pool — waking one and timing out on 63 looks exactly like a hang) and **RETAINS** the
CSA rather than freeing it under a live client. **The classifier is extracted** into
`src/nsfreqx.c` and host-pinned (the Stage-0c obligation, the way ufsd did into `ufsd#asv.c`);
**both** STCs now call it instead of each carrying a hand-written copy, and the client seam maps
a full pool to **`ENOBUFS`, not `ESHUTDOWN`** — a healthy stack with no slot right now should
make an app retry, not give up. Host **2907 PASS / 0 FAIL** (TSTREQX 58→119; the `asid-1` index
is pinned by a fake ASVT with a neighbour on both sides and **verified to discriminate** — the
suite goes red 4-fail with the index deliberately wrong; `nsfreqx_slot_action` is swept over all
**60** input rows asserting **exactly one** dispatches). **Offline gates:** `as370 -a=` listing
checked (`CS`→`BA34 A000`, base R10 **not** dropped to 0; stride `41A0 A860` = 2144) and **all
480 source statements verified present in the listing** — the direct check against a column-72
continuation swallowing one, stronger than a spot diff; column 71 clean; size asserts **verified
to actually fire** by breaking one; header mirror ↔ asm EQUs cross-checked programmatically 24/24;
alias scan 214 unique ≤8; clean build 6 modules + 49 test modules, no warnings.
**VALIDATED LIVE on MVSCE** (the stand was restarted mid-round and came back with the CTCI
pair working; the figures below are from the FINAL round, re-run in full after the review fixes
touched the reap path): full Stage-0
**`TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH` 444 PASS CC 0 batch+TSO** at the new layout
(**`TSTMVCD` deliberately excluded** — it is b0's probe, not a Stage-0 gate, and issue #53
makes it fail for environmental reasons, which in a full-green round costs a re-run and erodes
confidence). **`TSTRQXF` 106 PASS CC 0 batch+TSO** carrying b3's own gates — **(C)** `ver=2
nslots=64 guards=64` plus the **STRIDE CROSS-CHECK** (the routine is told to change slot **3** —
not slot 0, where no stride is exercised at all — and the test reads slot 3 back through the C
struct; `SLOTLEN` in asm and `sizeof(NSFV_SLOT)` in C are two hand-maintained numbers in two
languages and **nothing else in the suite compares them**), and **(D)** the three §7 checks:
**reuse `0/0/0/0`** (0,1,2,3 would mean release is broken), **skip → slot 5** with 0–4
pre-claimed (what proves the scan is a scan and not a constant; each pre-claim asserted, since
the `SLOT` probe verb is a `CS` precisely so this is an observation), **exhaustion `rc=16`** =
NOBUF with `inflight before=0 after=0`, every slot left exactly as found, and the anchor's
`exhausted` counter ticking **0→1 then 1→2** across the batch and TSO runs — which is also what
proves it is the anchor's counter and not a local. **`TSTRQXM` batch CC 0, 32/32**, with
`samples/host/shortwrite_listener.py` verifying **9353 bytes byte-exact** on the wire (the TSO
re-run FAILs by design — the one-shot listener was consumed by the batch run, so CONNECT and its
dependent CLOSE fail; batch is the gate, the TSTTCPW precedent). Startup is **clean with the
device present** — `NSF210I CTCI 0500/0501 UP ... MTU 1500`, `NSF211I INTERFACE LNK1 UP` — and
reports **`NSF055I CSA POOL 137264 BYTES (64 SLOTS X 2144) -- LARGEST FREE BLOCK NOW 905216`**,
which closes the sizing gap b0 left (it measured total and a *floor* on contiguous, never what
was free); it refuses to start naming the size it wanted. `SVC 239` stolen and restored by both
STCs, `IEF404I`, **no dump**. **The per-slot death guard ran live against the PRODUCTION STC**
(ORPHAN requests reaching NSFS): two **`NSF050I ... REQUEST REAPED`** rows and one
**`NSF051W ... REQUEST HELD`** — so the corrected two-move `CS` reap and two of
`nsfreqx_slot_action`'s rows are live-exercised, not only host-pinned. **Re-confirmed after the
review fixes**, which is what proves `nsfreqx_reap_ok` governing every reap live: the DEAD rows
still reap (the predicate permits) and the UNKNOWN row still holds — and TSTDEATH, which drives
the same guard through the probe STC, is CC 0 in the 444.
**A BLOCKING BUG FOUND BY REVIEW, NOT BY A TEST, AND FIXED:** `nsfsx_stop` called
`nsfsx_router_unload()` **unconditionally**, before deciding whether to retain the CSA — and
that unload `freemain`s the storage the SVC routine was `__loadhi`'d into, while a client that
failed to drain is parked in a `WAIT` **inside that code**, supervisor state, key 0. Retaining
the anchor while freeing the code is *strictly worse* than leaking both and contradicts the same
safe-side asymmetry the function is built on; the probe STC gets this right and was the stated
model. The unload now sits in the drained branch. **No gate covers this**, which is the point —
see gap (2). **TWO THINGS b3 DOES NOT PROVE, stated plainly:** (1) **two clients in two address
spaces racing on the same slot word** — the `CS` makes it true by construction, and construction
is not a live gate (**b4**); (2) **the `nsfsx_stop` retain branch and its nudge-all-slots loop
are live-UNEXERCISED** — `inflight` reached 0 before every stop (the reaps and the probes' own
`UNSTAGE` give it back), so the drain returned immediately and freed. Two attempts to induce it
were made and both self-cleaned; inducing it reliably needs a client that leaks `inflight` and
does **not** clean up, which is a test change and a deploy cycle — a defensible b4 item, and
exactly the branch the blocking fix above lives in. **BOTH GAPS ARE CLOSED BY b4**, which is
what b4 was for: contention measured live from both ends, and the retain branch reached through
the `PARM='LEAK'` induction. The qualifier above stands as written because the countersign
signs the sentence that says what was open — it is not softened retrospectively. Also worth naming: **`nsfsx_pending()`
early-returns on `g_busy`** and never scans for other PENDING slots, so a second client's
published request is invisible to the WAIT gate while the first is in service — it rides the
heartbeat instead of the probe. Latency, not corruption (ADR-0025 defect (2)'s shape), and a
state b3 created. A **measured** change worth keeping: a client that faults in the write-IN
direction now leaves its slot **`CLAIMED`**, not FREE, and the death guard never reclaims it —
**because `nsfreqx_reap_ok` excludes CLAIMED explicitly, NOT because it cannot be classified.**
(A first draft of the rationale claimed the exclusion was redundant with the classifier; it is
not — identity is recorded at the **claim**, before staging, so a CLAIMED slot has a real ASCB.
Review caught it, and it mattered: a rule believed redundant is a rule someone deletes.) Because
it *is* classifiable the leak is **closable**, but not by widening the predicate — the two-move
reap proves exclusivity with `CS(observed→CLAIMED)`, which succeeds trivially when the observed
state already IS CLAIMED, so it needs a distinct fourth state `CS(CLAIMED→REAPING)`; that belongs
with fault recovery, still open per ADR-0039/0041. **One rule, one encoding:** every production
reap is now gated on `nsfreqx_reap_ok` (it was pinned seven times and called nowhere, while the
live path expressed the rule twice), the predicate carries **both** reclaim reasons — a DEAD
client and untrusted storage — and TSTREQX sweeps all 60 rows asserting **every reap
`slot_action` MANDATES is one `reap_ok` PERMITS**. That sweep earned its keep immediately: it
failed twice before passing, first catching a contradiction I had written (untrusted storage must
**not** reclaim an UNKNOWN client — `HELD` already guarantees "never POST through this slot"
without freeing anything, which is the whole reason UNKNOWN is a third state), then forcing the
invariant to be stated correctly as an **implication, not an equality** (the two helpers answer
different questions: per-pass dispatch vs. permission, and equality failed on 14 of 60 rows that
are all correct). **#53, #54 and #56 deliberately untouched.**
**M5-2b4 (contention, the retain branch, and the WAIT-gate probe) — DONE, live-green;
maintainer-countersigned (PR #65).** The last sub-step of M5-2b; **M5 stays in progress
and no milestone flips.** Three things, and the first needed instrumentation before it could be tested at all.
**(1) Contention.** A failed `CS` on a slot word is **invisible from outside the routine** — a
client that finds slot K taken and moves to K+1 is externally identical to one that found K free,
lost the compare, and moved on, and to one that simply started at K+1. So the anchor header gained
**`collisions`** (one per failed `CS` in a claim scan) **plus one reserved word** — b3 left no
slack, which is the whole reason one diagnostic word costs a Stage-0 round. Header 48→**56**,
slot array moves by 8, `NSFV_ANCHOR_VER` 2→**3**, slot internals unchanged, `NSFV_ANCHOR`
137264→**137272**. The counter means exactly *"a claim attempt found a slot that was not FREE at
the instant of the compare"*; it **cannot** separate "already busy" from "lost a simultaneous
race" (`CS` reports only the value it found), and sharpening it needs a load before the compare
and a second counter — that is what `rsvd0` is for, deliberately unspent. The increment is a
**plain `L`/`LA`/`ST`, not the `CS` loop `exhausted` uses next door**: this is the routine's one
hot loop and a lost update makes a diagnostic under-report, never over-report. (Worth knowing:
the stand runs Hercules `NUMCPU 2` and **both CPs execute**, so a simultaneous compare is
physically possible here, not merely defended against.) **THREE independent witnesses**, because
"two clients ran and both were served" proves nothing: `collisions` moved (SOLO asserts the delta
is **zero** — the negative control, in the same binary); **A was given a slot other than 0** (the
scan takes the lowest FREE slot, so a solo client gets 0 forever); and the strongest — **A was
REFUSED while exactly one slot was free**, which with A holding no slot between its own requests
has exactly one cause: the other address space was occupying it. **No slot double-held** needs no
counter: every request carries a 64-byte `NSFRQE` image *through* the slot and back and the STC
writes only the six result fields, so each client stamps a per-request identity (`reqid`,
`sockdesc`) and asserts the image returned is its own. **(2) The retain branch.** A blocking
`RECV` cannot induce it: `nsfsx_stop` clears `ANCHOR_ACTIVE` **before** it nudges, so a nudged
client takes the routine's `WQUIES` path and **drains itself** — b3's two failed attempts, explained.
What reaches it is a slot left **CLAIMED** with the count taken and **nobody on its reply ECB**;
a client that faults in the write-IN move leaves exactly that, so the induction is that fault with
the cleanup withheld. **The `nsfsx_router_unload` fix is proven FORWARD, not by revert** — reverting
means freeing CSA a task is executing in, so the failure mode would be the system — and that is
**weaker evidence, labelled as such**. **(3) `nsfsx_pending`.** b3 asked for "scan all slots";
taken literally that is a **hot spin**, because `evt_mainloop` skips its WAIT whenever a probe
answers non-zero and the drain's dispatch arm needs the one private `NSFRQE`. Split by what the
outcome NEEDS: `REAP`/`HOLD`/`REAP_BAD` finish inside the CSA slot and never POST, so they are
consumed whether or not one is in service; `DISPATCH` stays invisible until it frees. New pure
`nsfreqx_actionable` (`NSFRXABL`) is the ONE encoding both the drain and the probe ask through —
the drift *is* the spin. It fixes a real hole (a second client that published and then **died**
sat un-reaped for as long as an unrelated blocking op ran) and deliberately not the starvation,
which needs **concurrent service** (still open). Writing it found a bug: **the in-service slot is
still PENDING**, so a scan-while-busy would have reaped a dead client's slot *from under the
executive* — and reaping clears `stage`, which is what the parked request's `ubuf` points at
(cross-AS use-after-free); the selector skips it explicitly. **§4:** `nsfreqx_reap_ok`'s UNKNOWN
rationale replaced — "HELD already prevents the POST" is equally true of LIVE, and the LIVE row
deliberately reclaims a live client's storage; what separates them is the **standing of the
evidence** (with LIVE the identity was corroborated against the ASVT; with UNKNOWN the
storage-trust judgement rests on the same unreadable ground). Comment only.
**Offline gates:** `as370 -a=` listing checked (`L/ST` at displacement `X'030'` off R2, `CS` still
`BA34 A000` with base R10 **not** dropped to 0, stride still `41A0 A860`) and **all 962 source
statements present byte-identical in source order** — **verified to discriminate**: a deliberate
over-long comment makes as370 emit `LA R3,1(,R3)R3,ANCCOLL(,R2)` and the check catches it. Column
71 clean; size assert **verified to fire**; alias scan clean (217 unique ≤8, one new `NSFRXABL`);
host **2846→2925** (TSTREQX 119→**137**, and **verified to discriminate** — 3 fail with the
anti-spin rule broken). **VALIDATED LIVE on MVSCE:** Stage-0 `TSTSVC`/`TSTMVCK`/`TSTUBUF`/
`TSTDEATH` **444 PASS CC 0 batch+TSO** at the new layout (`TSTMVCD` excluded, #53); `TSTRQXM`
**batch CC 0 32/32** with the host peer verifying **9353 bytes byte-exact**; `TSTRQXF` **53/53 CC 0
batch+TSO**; startup `NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144)`. **The two-client gate:
A CC 0000 13/13, B CC 0000 8/8** — mechanism (phase 1) **150/150 on slot 1**,
`collisions` **0→150** on a fresh anchor, B's index never anything but 0; **the gate** (phase 2)
**A served 239 / refused 2761 / wrong 0** while **B was served 194 / refused 154** — both refused
by the other and both winning the slot, the interleaving measured from both ends — with
`exhausted` **0→2915** and 2761+154 = **2915 exactly**; pool **64/64 FREE** at exit. SOLO on the same instance minutes before read `collisions 0→0`; the
probe STC's own stats independently read `COLL=0` over 126 sequential requests (a second negative
control, different code). The first gate run, on the previous instance, gave the same verdict from
a non-zero baseline (`9888→10038` against SOLO `9888→9888`). **The retain branch RAN:**
`NSF043I SVC 239 RESTORED` then **`NSF054W 1 CLIENT(S) STILL IN FLIGHT -- CSA AND SVC ROUTINE
RETAINED`** 10 s later; the anchor read back afterwards still carried `NSFVANCR` with `ACTIVE`
clear and `inflight = 1`, and the next `S NSFS` came up on a **different anchor AND a different
router EP** (00A8B248, was 00A820C8) — the exact evidence the module was retained; the largest
free CSA block also fell 1073152→933888, which is **consistent with** pool + module but no more
than that (`nsfsx_csa_largest` refines only to 4 KB, so both figures carry ±4 KB). **The gate's
second run needed an external wake floor** — it crawled at ~3 requests/min until a continuous
host `ping -i 0.2` was started (the #64 latency defect, not a pool property); the first run and
every other gate needed nothing. **Zero dumps across the whole round** (the one `S806`, on
`TSTRQXCL`, was self-inflicted: `make test-mvs --only` had replaced TESTLIB with the Stage-0 four,
so `TSTRQXC` was not there). **No module source changed after `make deploy`** — the three later
commits touch only `test/mvs/tstrqxc.c` and docs, so every live figure is about the deployed
binary (§5's most expensive failure class, checked rather than assumed). **A finding filed, not fixed (#64): the executive can sleep through a published
request** — one sat 11 minutes, and a `F NSFS,STATS` queued in between was answered in the *same
second*, which is what identifies the sleeper as the STC. It is **conditional** (a freshly started
STC served 8 sequential requests in 0.39 s with nothing else running), **pre-existing** (the new
probe is strictly more permissive than b3's), and two asymmetries against Phase 1 are the starting
points: `nsfsx_drain` never resets its wake ECB where `nsfreq_drain` does, and the
`nsftmr_plat_arm(1u)` heartbeat does not survive an empty timer queue. **Two defects in the gate
itself, both mine:** phase 2 **starved itself** by backing off 10 ms per refusal (served 0/150 —
which is a real property: *the scan is not a queue and makes no fairness promise*), and A asserted
the whole pool was clean **while B was still using it** (that check belongs to whoever leaves
last). **TWO THINGS MUST HAPPEN BEFORE (e):** **#64 is a prerequisite, not housekeeping** — an executive
that does not wake without device traffic makes any throughput/latency number from a stress round
meaningless, and (e) measures exactly those; and **IPL before (e)**, because the induction's ~137 KB
+ router leak until IPL, so b3's `LARGEST FREE BLOCK NOW 905216` no longer describes this stand
(1073152 before the induction, 933888 after) and (e) would otherwise size itself against an
artificially small pool. **One acceptance item was SUPERSEDED, not met:** "B's request visible to the WAIT gate while A is
served" is exactly the spin under serialised service, so what replaced it is the non-dispatch
outcomes — read the list against a green round without this line and it looks met. **b4 does NOT
prove:** concurrent service; hardware arbitration of a simultaneous `CS` — and phase 1 is positive evidence that no lost race
occurred *in that phase*, so on this stand it is unobserved, not merely unmeasured; **reaping a second
client's slot WHILE a request is in service** — the very hole the probe fix closes went
LIVE-UNEXERCISED (no `NSF050I`/`NSF051W` from NSFS all round, because nothing died mid-service;
the decision is host-pinned and wired, and b3's live reap evidence was collected with nothing else
in service); **that the nudge loop reaches more than ONE parked client** (the induction leaves
exactly one, so the 64-slot loop found one — b3's change over the probe's single-slot wake is no
better exercised than before); 2-AS stress (**e**); fault recovery / the CLAIMED-slot leak; or
#64.
**M5-2c0 (the last unwindowed key-0 write-out) — DONE, live-green in all three revert
states, COUNTERSIGNED by Mike 2026-08-26 (PR #66).** The first sub-step of M5-2c;
**M5 stays in progress and no milestone flips** — `#64`, c1, c2, (e) and c3 are ahead,
and an IPL is still owed before (e) (b4's induction left ~137 KB plus the old router
module retained). **Obligation #1 — the write-out key window on `ubuf` — is now CLOSED
on BOTH destinations**, the RQE path (b1) and the XFER path (c0); the obligation as a
whole is restated in three categories below, with category 3 open and homed in (d).
`XFEROUT` was the last read-out in the SVC routine that stored into a caller-supplied
`ubuf` under **PSW key 0**. b1 closed that window on the RQE path
(`RQEOUT` → `MOVEOUT`) and left `XFER` alone on a recorded assumption — that (c) would
**delete** the verb. **The assumption had expired:** deleting `XFER` retires `TSTUBUF`, the
only gate that proves the keyed `ubuf` bounce, so the verb survives to **c3 at the earliest**,
after (e). The read-out now moves through the same `MOVEOUT` window, which becomes a
**three-call-site block and not a second key-borrowing block** — the property that keeps
"what executes under a borrowed key" a question with a short, checkable answer.
**The kickoff's "three lines" estimate was wrong, for three reasons worth keeping.**
(1) `XFEROUT` did not move with `MVC` — it moved with the **raw `D9` `MVCK`** capped at
`MVCKMAX` 255, so the loop is REWRITTEN: cap `MVCMAX` **256**, true piece length in `R0`,
`BCTR` for the `EX` length-1, source-key set-up dropped (a 2048-byte `XFER` now crosses as
**eight 256-byte pieces, not nine of 255**). (2) The `SPKA` operand set-up is **nine
instructions inside `RQEOUT`**, not a shared block, so it is duplicated **inline and
byte-identical** — the countersigned production path keeps an **unchanged instruction
stream**, which the diff proves outright; the copy expires with the verb in c3. Extracting a
shared `KEYWIN` was the cleaner code and was rejected on review cost, **after** measuring the
one thing that could have forced it: everything addressed off `R6` (`MOVEOUT`, `SLOTADR`,
every probe handler and bail path, the whole literal pool) must stay inside the single
`USING NSFVSVC,R6` range, and the module ends at **`X'56C'` = 1388 bytes against 4096**, so
the ~36 added bytes are free. (3) **`XFEROUT` is not reachable under the production STC** —
established by ENUMERATION, not by reading the arm that obviously rejects it: `nsfsx.c` has
**exactly one** `NSFV_REQ_DONE` assignment, guarded by `g_busy && g_busy_slot && POSTED`;
`g_busy` is set at exactly one place under `if (ok)`; and `ok` is set at exactly one place —
the `ACT_DISPATCH` arm's RQE branch, whose `else` sets `HELD`. Other arms reach `FREE` or
`HELD`, `nsfsx_next_actionable` skips anything not `PENDING`, and the shutdown nudge
(`nsfsx_wake_parked`) POSTs without touching `req_state` after `nsfsx_stop` has cleared
`ACTIVE` — so a nudged XFER client wakes on `HELD` and takes `WQUIES`. Only `nsfv.c` sets
`DONE`, which the client's post-`WAIT` path tests before reaching `XFEROUT`. So the gate had to run against **NSFV, not
NSFS**, and **ADR-0041's "reachable by anyone who can issue the SVC" is corrected**: the verb
is *dispatchable* by any unauthorised caller, but the key-0 write-out only ever executed under
the probe STC. `XFERIN` is **untouched** (its source-key-8 `MVCK` is the mechanism `TSTUBUF`
proves). Anchor layout unmoved, `ANCVERNO` stays **3**, `NSFRQE` still frozen at 64 B,
`include/nsfvsvc.h` unchanged — apps relink only.
**The gate is a new file, `test/mvs/tstxfw.c` (`TSTXFW`, host = false), and it has TWO
parts because neither alone pins the window.** `TSTUBUF` does **not** discriminate — it is
green on unmodified code, since a key-0 store into its key-8 buffer succeeds. The **positive
control** (an ordinary `XFER` of 300 bytes into the client's OWN key-8 buffer, `+1`
byte-exact, guard byte after `ulen` untouched; 300 deliberately not a multiple of 256, so the
short last piece exercises the `EX` arithmetic) exists because **the fault check alone is
satisfied by a window that faults on everything** — a misread `TCBPKF` borrowing a wrong
non-zero key would pass the gate while breaking every real client. The **gate** points `ubuf`
into the anchor's OWN staging buffer (`&slots[0].stage[1024]`, 64 B) — key-0,
non-fetch-protected storage, the one class that gets PAST the key-8 read-in — so a window
that failed to take performs a harmless staging-to-staging copy rather than a clobber, and it
is self-validated by three independent facts first (eyecatcher `"NSFVANCR"`; a key-8 READ
that **succeeds**; a key-8 STORE that **faults**). A fault alone is not evidence (b2's first
attempt faulted after the POST and every gate went green over it), so an **untouched `rc`
sentinel** is asserted alongside it. **Offline gates:** `as370 -a=` listing checked —
**no `D9` byte left in the read-out** (three `MVCK` sites remain, all write-in), `BAL` to
`MOVEOUT` = `45F0 63C8` base **R6** not dropped to 0, **`SPKA` exactly twice in the module
and both in `MOVEOUT`**, `IPK` exactly twice (one per read-out); **all 995 source statements
present byte-identical in source order** (verified to discriminate — a deliberate over-long
comment makes as370 emit a merged statement and the check catches it); instruction-stream
diff **confined to `XFEROUT`**; column 71 clean; 6 modules + 51 test modules cross-link clean;
alias scan clean (TSTXFW exports only `main`); host **2925 PASS / 0 FAIL** unchanged — and
that host figure is a **no-regression check only**, evidence of nothing else, since
`asm/*.asm` never compiles on host and the new test is `host = false`.
**VALIDATED LIVE on MVSCE, all three states**, unauthorised client (`TESTAUTH == 0`), anchor
`00AAD7C8` chased `CVT 0001D048 → SCVT 0001D510 → SVCTABLE 0000FA60 → svcepa 00A8B248`:
window **IN** → `___try` rc **`000C4000` (S0C4)**, sentinel `5AC0F001` untouched, **0 of 64**
target bytes changed, **20/20 CC 0 batch+TSO**; window **OUT** (`SPKA` pair commented out and
**verified gone from the listing** — `MOVEOUT` reduced to `EX` + `BR`, zero `B20A` in the
module) → rc **`00000000`**, the request **RETURNED `rc=0`**, sentinel overwritten,
**64 of 64** bytes changed, **18/20 CC 1**; window **RESTORED** → `S0C4` again, **20/20 CC 0**,
listing byte-identical to the window-in listing apart from the timestamp and the **object deck
byte-identical**. **The positive control PASSED in all three states**, which is what makes the
middle row "the window is absent" and not "the borrowed key is wrong". The content
observation **does** discriminate here (the `+1` transform makes the round trip
non-idempotent, unlike `tstrqxf.c` (B) where the moves are exact reverses) and is still
**printed, never asserted** — whether a protection exception on `MVC` suppresses or terminates
is not pinned on this target, so a **partial** store is not excluded.
**It cleans up after itself and ASSERTS that it did:** the fault sits after the STC replied,
so it leaves `slot0 state=2 inflight=1` (measured — exactly the OUT-direction shape b1
predicted); `CLAIMOK` stores the claimed index into the caller's block before the fault, so
the test `UNSTAGE`s that slot and asserts `state=0 inflight=0`. Without it every run would
cost a slot and force `P NSFV` to retain the whole anchor to IPL. The dangling state itself
stays **reported, never asserted** — write-out fault recovery is still the open M5-2 item
ADR-0039 names. Three-state return code (0 ran+passed / 1 ran+failed / **20 could not run**),
the `TSTRQXF` contract. **Round:** NSFV `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH`/`TSTXFW`
**484 PASS CC 0 batch+TSO** at the restored module (`TSTMVCD` excluded, #53) — and `TSTUBUF`
green is the **positive-direction control at round scale**, not a regression datum. NSFS-side
**confirmation, not requirement** (the RQE path's bytes are unchanged): `TSTRQXM` **batch
CC 0 32/32** with the host peer verifying **9353 bytes byte-exact**, `TSTRQXF` **53/53 CC 0
batch+TSO**; **`TSTRQXC` was NOT run**. `SVC 239` stolen and restored by both STCs, **no
dumps**. **The write-out obligation now reads in three categories:** (1) `ubuf` — closed
(b1 + c0); (2) `rqeimg` — closed (b1); (3) the **20** unwindowed key-0 stores into the
caller's `NSFV_REQ` block (`ST R…,REQ*(,R8)`, counted from source) — **open**, gated only by
the `"NSFV"` eyecatcher check on `R8`, which validates the pointer and not the key, so
**lower risk, not none**; its home is **(d)**, as ONE validation of `R8` rather than twenty
scattered `SPKA` windows. **c0 does NOT prove:** suppressed-vs-terminated on the faulting
`MVC`; anything about the RQE path beyond confirmation; recovery from a write-out fault; or
the reachability finding's own consequence — that an unauthorised caller can dispatch a probe
verb at NSFS and have it consume a slot that is never revisited, which is filed separately
and belongs to (d)/c3.
**64-1 (the wake-ECB reset, and the experiment it makes possible) — DONE,
live-green in all four deployed states, **maintainer-countersigned (PR #72
merged)**. It does NOT fix issue #64 and does not close it; #64 stays OPEN.** Not a milestone step: `#64`'s investigation runs
alongside M5-2c, and c1, c2, (e) and c3 are still ahead. `nsfsx_drain` never
cleared `g_wake_ecb` — assigned zero once in `nsfsx_start` and never again — so
the first cross-AS POST **latched** the POSTED bit for the life of the STC, and a
posted ECB in the ECBLIST makes `WAIT` return immediately: `evt_mainloop` could
not block afterwards. Phase 1's `nsfreq_drain` has always reset `g_reqecb`
before taking its queue and so does the probe STC; **Phase 2 was the one that
diverged** from the discipline `src/nsfevt.c` step 2b states in so many words
(ADR-0022 annotation). **The diff is ONE statement** — a comment-stripped diff
of `src/nsfsx.c` against `main` is exactly `+ g_wake_ecb = 0u;`, checked
mechanically rather than by reading. The reset sits **ahead of both scans**, and
that position closes the window completely: a client publishes (`req_state =
PENDING`) and only THEN POSTs, so a POST before the reset implies the publish was
too and both scans run after it, while a POST after it leaves the bit standing
for the WAIT — **there is no third ordering**. **Phase 1's `do/while` recheck loop
is deliberately NOT replicated**: the recheck half already exists one level up in
the WAIT gate, asking through the same `nsfsx_next_actionable`, and a loop here
would spin on the **no-progress `__super` return**, would have **no finiteness
argument** (a client may republish the instant it is replied to — Phase 1's rests
on its bounded fan-in and does not transfer, and an unbounded drain starves the
timers and devices sharing the pass), and would turn ADR-0042 §10's one unit of
work per pass into "serve every inline-completing request per pass".
**Gate 1, three deployed states, one assertion moving** (STC 1480/1481/1482/1483;
the "before" arm cost **zero deploys** — 64-0d left 64-0c's binary in
`NSF.LINKLIB`, confirmed by *field*): reset ABSENT after 8 requests →
`POSTED=Y`, **7 482 passes/s, 25.9–30.5 % of a host core**; reset PRESENT →
`POSTED=N`, **9.98/s, 0.7–1.6 %**; reset commented OUT and redeployed (source
verified **identical to `main` at instruction level**) → `POSTED=Y`, **8 532/s,
26.0–26.9 %**; RESTORED → `POSTED=N`, **9.99/s, 0.5–0.7 %**. Every SOLO run CC
0000 8/8 **inside one console second** in every state — the reset costs no
service. **This round's deploy-took-effect check had to be a new shape**, because
64-1 adds no field: **`POSTED=N` with `SERVED` non-zero IS the proof** (before the
reset that combination was impossible); its complement is **ambiguous** and is not
used as a check, so the revert arm is corroborated by the deploy output read for
the mid-chain `HTTP 500` and by the instruction-level diff. **A SHA-256 of the
load module is NOT usable as evidence and the check relying on it was dropped:**
two builds of byte-identical source differ in **exactly two bytes** (a build
timestamp at offset 10560), measured by building twice. **`WAKEPOSTS` changed
meaning** — it counted a latch (tracking `EVTPASSES` at a constant offset of
3 755), it now counts **wake events**: `WAKEPOSTS == SERVED` exactly at 8/8,
28/28, 36/36, and at scale it falls **below** `SERVED` (164 257 vs 164 570), which
is the documented coalescing that makes it a lower bound on posts, not a tally.
**Every figure in `docs/nsf-64-0*.md` was taken under the old semantics** and is
not comparable — noted at the declaration, where such a reader looks.
**THE FLOOR IS MEASURED, ON PURPOSE, AND WAS NOT BUILT.** After `TSTRQXM` (TCP:
connect, 9 353 bytes of short writes, close) the TCP timers cancel and ADR-0034's
*queue empty ⟺ STIMER disarmed* takes the heartbeat away: **`EVTPASSES` 5854 →
5855 across 259 s — one pass in 259 seconds**, host CPU 0.2–0.7 %, against 9.98/s
on the same instance minutes earlier. **Paired with service in the same breath**,
because `EVTPASSES ≈ 0` alone reads as a dead executive: on that same floorless
instance 8 cross-AS requests were submitted and **served and returned inside one
console second**. So **the wake works with no floor** — "no floor is needed" is a
measurement for the HEALTHY case, and says nothing about #64. **Functional
regression** on the final restored module (MBTTEST JOB02334): `TSTRQXC` **CC 0
batch+TSO 8/8**, `TSTRQXF` **CC 0 batch+TSO 53/53**, `TSTRQXM` **batch CC 0
32/32** with the host peer verifying **9 353 bytes byte-exact** (log mtime checked
fresh, listener confirmed in `LISTEN` first) — its TSO re-run FAILs **by design**,
the two failures being `CONNECT` and its dependent `CLOSE` after the one-shot
listener was consumed by the batch run (the TSTTCPW precedent; batch is the gate).
`TSTRQXM`'s `CONNECT` is the first **parked** request to complete, so the parked
path — the one a lost wake would break — is exercised on the reset build. NSFV
round (`P NSFS` first — both STCs steal SVC 239) `TSTSVC`/`TSTMVCK`/`TSTUBUF`/
`TSTDEATH`/`TSTXFW` **all CC 0 batch+TSO, 484 PASS / 0 FAIL** (`TSTMVCD` excluded,
#53). Host **2925 PASS / 0 FAIL**, unchanged — and that is a **no-regression check
only**, evidence of nothing else, since `src/nsfsx.c` is MVS-only.
**GATE 2 — the #64 reproduction attempt — did NOT reproduce, and that is WEAK
evidence, for a reason that can be named.** 64-0b's standing instruction was that
64-1 must attempt one, because it becomes untestable the moment the reset lands.
The arm ran at scale: one instance, `TSTRQXM` then the two-client contention gate
**45 times back to back**, no `PARM='LEAK'` — 42 A CC 0000 (3 CC 0001, all the
gate-INTERNAL "A was given a slot other than 0" timing assertion, which the gate
itself calls the weaker witness; its decisive refusal assertion passed every
time), 45 B CC 0000, `SERVED` 69 → **164 570**, `EVTPASSES` → 320 733 (**≈ 1.95
passes per request**), plus seven idle windows totalling **2 356 s** (the
longest 972 s, over which `EVTPASSES` moved **320 733 → 320 736** — three
passes — with `SERVED` frozen). **No stall; the detector, armed 31 m 16 s,
emitted nothing — and no false positive either**, which matters because a naive
EJST-flat detector would have fired almost continuously against the floorless
executive. **The detector had to be REBUILT and then VALIDATED before its
silence could be quoted:** the reset destroys 64-0c/0d's `ASCBEJST`-flat detector,
because a correctly idle executive with no floor is *exactly as flat as a stalled
one* — so the criterion became the conjunction **EJST bit-identical AND a slot
`req_state == PENDING` AND `served` frozen**, all CSA/SQA through `/.dm`, never a
MODIFY (which POSTs the cib ECB in the executive's own ECBLIST). Its read path
was then proven live rather than trusted — a census during the campaign returned
`CLAIMED` 15 and 18, a `HELD`, and all-FREE, and fast-sampling slots 0–3 caught
**`PENDING` in 3 of 360 reads** — so a silent detector is a real null and not a
wrong stride (CLAUDE.md §8.5, closed rather than argued). **The honest statement
of the null is not "45 rounds and no stall":** every stall on record shares ONE
condition, a client **parked with a published request**, and that same sampling
measures how much of this arm had it — **`PENDING` in 3 of 360 reads ≈ 0.8 %**, sampled over
~90 s of the campaign on slots 0–3, during the heaviest workload the stand can
produce — where a single 64-0d gate round held it for
MINUTES, because those rounds were themselves slow. The condition was present for
a few seconds in total. **The idle arm cannot fire this detector at all** (no
parked client ⇒ never `PENDING`) and 64-0c already called idle non-reproduction a
property of the configuration; it is reported for its `EVTPASSES`/CPU content, not
as a fair test. **What is NOT weakened is the prior finding:** 64-0c measured the
executive **not waiting** during a stall and 64-0d measured its address space
**stuck part-way through an MVS swap-out** (`OUCBQFL = X'80'`, `OUCBGOO`), in
fields NSF does not write and cannot see — **the suspension is outside nsf370**,
the reset was never a candidate fix for it, and this round neither confirms nor
refutes it as a candidate *provocation*. **An uncontrolled observation, offered as
exactly that:** M5-2b4's gate "crawled at ~3 requests/min until a continuous host
ping was started"; these 45 rounds needed no external floor and each ran 3 000
attempts in ~10 s — months and many changes apart, nothing held constant, **not
counted as evidence**. **Round hygiene:** the stand's master console was dead on
arrival, stuck in `*IEA420A NO FULL CAPABILITY CONSOLES, REASON=EXT` — 64-0d's own
`ext` residue, which needs a SECOND press — and the symptom is worth knowing
because it is not the obvious one: commands were **accepted and queued and never
executed** (mvsMF returned a response key for each) and the 1403 hardcopy simply
stopped; a second `ext` gave `IEE143I CONSOLE SWITCH` and **flushed ten minutes of
queued commands at once**. An IPL was offered and **not needed**. A first CPU
sample read 17–20 % and was **my own instrument** — a detector reading 64 slots
through HTTPD *inside the guest* every 2 s; stopped, the same instance reads
0.7–1.6 %, and the detector now pre-filters. `IGF991I`/`IGF995I` on device 500 —
the familiar CTCI degradation, not a new symptom. **`#64` STAYS OPEN**; it was
found CLOSED (COMPLETED) at round start, five hours after its own reopen, and was
reopened with a comment saying plainly that nothing has fixed it. **It was also
RETITLED at the countersign**, because both halves of its old name — *"NSFS
executive can sleep through a published request: the transport's wake has no
floor"* — are now false by measurement: no floor is needed (above), and the wake
is not the defect (64-0c refuted a committed WAIT from one side, 64-1 confirms
the POST from the other). It is now **"NSFS address space stalls mid-swap-out:
tasks non-dispatchable while `OUCBQFL = 80`"** — 64-0d's finding, which is what
the issue always described. A comment records why, so the rename is legible
rather than mysterious. **64-2 — "the floor, if 64-0/64-1 show one is still
needed" — is therefore CLOSED AS UNNECESSARY, not deferred:** the wake works with
no floor at all, measured twice at two idle scales (one pass in 259 s, three in
972 s, with eight cross-AS requests still served inside one console second). It
is recorded here rather than left on the plan, so nobody reads the plan later and
builds it; the reasoning is ADR-0043's, not this line's. **The next question is
the one 64-0d left:** what the swap-out transition is pending on — outside
nsf370's code — and the cheap untried instrument is **NSFV**, the probe STC, which
runs the same transport with **no device at all**. Whether NSF should mitigate an
MVS condition, and in what form, is Mike's call.
**64-0e — NSFV, the same transport with no device — DONE, docs-only, maintainer-countersigned
(PR #73 merged). It fixes nothing and #64 stays OPEN.** **Step §0 was answered from the logs
before any machine time and is the more useful half:** the parked client **PRECEDES** every one of
the nine recorded stalls — `served` is non-zero at every onset (it only advances on a completed
request and is frozen through a stall) and the client keeps publishing *during* each stall
(64-0d stall 1: `collisions` 241 430 → 2 822 998 while `served` is frozen, a ratio of **exactly
64.0000** twice over, i.e. all 64 slots non-FREE and ≈ 13 022 full scans). The EJST trace dates
that stall to the second — idle at 0.3 % of a CPU until the first request ≈ 10:43:37, a full core
to ≈ 10:44:06 while 341 requests complete — so the stream begins ≈ 29 s **before** the stall.
**But three things ran under one name and only two precede:** submitting **at rate** (precedes), a
request outstanding at onset (continuously true, not created by the stall), and a client **parked
for minutes** (the stall's own effect). **An arm built to hold one parked would have rebuilt the
consequence and passed as a reproduction** — which is why §0 was worth more than the round.
Also RETRACTED here: 64-0d's "impossible" slot rows are real (`FNSLOT`/`DOSLOT` stores no owner
identity; `tstrqxc.c` pre-claims slots 1..61 through it), and 64-1's campaign is DATED —
904 s / 45 rounds = 20.1 s per slot, ~10 s load then ~10 s gap, so it **never entered** the
40–90 s window every stall fires inside. **The live arms:** NSFV 543 356 requests / 1 134 s /
max inter-step gap **1 s** / 0 slow steps of 10 004; NSFS (device up, post-reset, `POSTED=N` with
non-zero `SERVED` confirming the build) 139 240 / 988 s / max gap **1 s** / 0 of 17 403. **N(iii),
and what it earns is the WINDOW and only the window** — TWO explanations survive, since the arms
were not equally contended (10.5 % vs 1.1 % collisions per request) and neither reached the
saturation of the arm that reproduces. **The detector is PROVEN to fire on the phenomenon**, which
is what makes the nulls worth anything: 64-0d's stall 1 sits in the same cumulative console log as
one `TSTRQXC` step of elapsed `00:07:17.57` (start CEST 10:44:02 vs its recorded 10:44:10 →
10:51:19). **THE SWAP READING IS THE ROUND'S REAL OUTPUT, and it points at the read with the sign
REVERSED.** NSFS: **54 OUCB samples over 38 min, never once swapped**, including an *unplanned*
8-minute idle stretch the spool exhaustion handed over (12/12 resident); NSFV: 16 samples, **12
swapped out**, every transition COMPLETING (`0C` → `00` → `0C`, never a bit standing, and the
0.12 s probe INCLUDES the swap-in). That is a **matched** comparison, not an empty one — so the
candidate is not *"the read makes a swap-out get stuck"* but *"**the read makes NSFS
swap-resistant**"*, and the rare event in #64 is then not *"the swap got stuck"* but *"**SRM began
a swap-out at all**"*. **It stands or falls on one field:** the 45–60 s cadence cannot exclude a
fast out-and-back cycle, and the sampler did **not** read **`ASCBSTOR`** (which 64-0d measured
going `0F8BCC00` → `0F923C00` across a transition). Any next round on this axis carries it — it is
not an improvement to the instrument, it is the instrument. **"Untestable without reintroducing
it" was WRONG and is corrected:** the 64-1 revert build exists, was run four times and is
instruction-diff-verified against `main`, and this round's NSFS arm is already its paired control,
so the spin arm is a decision and not a technical obstacle. **Three faults of mine are in the
record**: I filled the JES2 spool (`$HASP355`, cleared by the maintainer); my first detector query
used a **string** compare on the MVS clock so `"10.01.26" < "9.36"` dropped everything after 10:00
and reported a clean null; and I asserted NSFS's concurrency from one `D A,L` instead of counting
it (2 address spaces for 940 of 989 s). Stand left clean — both STCs stopped, no `NSF054W`, zero
dumps, no CSA debt, TESTLIB holding `TSTRQXC` alone. Host **2925 PASS / 0 FAIL**.
[[nsf370-64-0e-nsfv-arm]]
**64-0f — the spin arm — DONE, docs-only, maintainer-countersigned (PR #75 merged). It
fixes nothing and #64 stays OPEN.** Not a milestone step. **A stall was reproduced and
FULLY MEASURED — once in four deliberate attempts**, which is the whole of what the round
claims. The only step in this investigation that deliberately deployed a module known to be
wrong (the 64-1 reset reverted, authorised for this round; **wall clock live: 1 h 14 min
49 s**, `main`'s module back and verified before the document was finished — `POSTED=N …
SERVED=16`, `EVTPASSES=345` against the spin build's 529 481, and `WAKEPOSTS == SERVED`
exactly, which is 64-1's wake-event semantics rather than the latch). **The kickoff's
premise was refuted from primary source before any machine time:** it rested on "the nine
stalls all occurred in **idle** windows", and `docs/nsf-64-0c-measurements.md` says the
reverse in so many words — *"An idle stack does not stall"* — with **five** idle
non-reproductions on record and every stall firing 40–90 s into a round. Run as written,
the round could not have fired, for two independent reasons: the arm never reproduces
there, **and the detector is structurally blind there** (no parked client ⇒ never
`PENDING`). Both shapes were run, labelled, idle first. **The reproduction, read before any
intervention and identity-proven** (`OUCBASCB` matched, never inferred): 64-0d's signature
field for field — `QFL=80[GOO] SRC=09 RCTF=00 DSP1=00`, `CPUS=0`, EJST bit-identical, the
client `MBTTEST` parked on `reply_ecb` with its wait bit set. **And then the reading the
round was built to take: it CLEARED BY ITSELF after ~12 minutes.** `ASCBSTOR` `0FAF3C00 →
0FC26C00` — the segment-table origin reassigned, the retrospective signature of a
**completed** cycle, which is exactly the field 64-0e identified as the instrument and could
not see — corroborated independently by **`OUCBSWC` 0 → 1** (`0000` on all 38 prior samples,
`0001` on this one: **exactly one** cycle, not a fast out-and-back series). `served` 33 → 49
and the client job ended **CC 0000** — it was never failing, it was parked. **So #64's
"stuck" is better read as "very slow, and observed to finish"**, and 64-0d's wording is
revised rather than contradicted: "stuck" means "not yet observed to complete", and this
round supplies one that did. **Had the STC been stopped to "recover" the stall, the answer
would have been destroyed.** **The detector fired on the live phenomenon** — the full
conjunction (EJST bit-identical **and** a slot `PENDING` **and** `served` frozen), which is
a stronger validation than any census: proven to fire on the thing whose absence it would
otherwise report. **What the round does NOT establish, and the list is not hedging:** it
does **not identify the provocation at all** — the onset coincided with a `make test-mvs`
deploy burst, and all three attempts to reproduce that were clean (sustained load **28 000
requests, no stall**; the identical burst after load; the identical burst after a matched
840 s idle), so **request rate is excluded (33 vs 28 000)** and nothing else is established;
it does **not make 64-1 the fix** (one positive against priors collected under different
conditions, and 64-1's own arm is dated); it does **not show the spin causes** the stuck
swap-out (the chain ends at `QFL=80[GOO]`, inside MVS, in fields NSF does not write and
cannot see); and it does not rule out that the stall was always reachable and simply rare —
**one success does not measure a rate**. **B1 (idle, spin) was completely healthy** across
2 × 400 s with `CPUS=1` on 17/17 and **no swap-out even attempted**, which is itself a
mechanism for why idle has never reproduced: SRM does not attempt to swap a **spinning**
address space. **An unexplained discrepancy is reported rather than explained away:** the
same defective source cost **112.7 %** of a host core here against 64-1's **25.9–30.5 %**,
and the instrument load runs the *wrong* way to explain it — a caution about every
cross-round comparison in this investigation, including this one's. **None of the three
predictions fired as written** (the closest, F(i), placed the stall in an idle window, which
B1 refutes). **Two decisions were brought back rather than taken, and both are ratified:**
arm 2 was skipped (its purpose is unattainable against a phenomenon firing one time in four,
and it is confounded) and the spool purge was left to the maintainer.
**A CORRECTION TO THE #64 PRECONDITION, recorded here: it SPLITS.** The blocking was
justified with "the sweep exists for an idle stack, which is the situation the defect
occupies", and `docs/nsf-64-0c-measurements.md:189` states the opposite in so many words.
So **c1 is RELEASED** — a sweep on an idle stack is not affected by a defect that does not
occur on an idle stack — and **(e) stays BLOCKED, more firmly than before**: the stalls fire
under exactly the load (e) measures, and a twelve-minute event inside a throughput round
makes every number in it worthless. [[nsf370-64-0f-spin-arm]]
**64-3-0 — the no-swap survey — DONE, docs-only, read-only, maintainer-countersigned
(PR #76 merged). It builds nothing and #64 stays OPEN.** Not a milestone step. The question
64-0f left: 64-0f measured the stall as a **completed** swap cycle (`ASCBSTOR 0FAF3C00 →
0FC26C00`, `OUCBSWC 0 → 1`, ~12 minutes non-dispatchable), so should NSFS be non-swappable at
all — the way essential subsystems are normally run. **Nothing was issued, changed, started or
stopped**: no `SYSEVENT`, no deploy, no PPT edit, no STC recycle, no job submitted; NSFS was
sampled as found (STC01493, the module 64-0f restored). **THE MECHANISM EXISTS.** `SYSEVENT`
is **not** in `SYS1.MACLIB` — it is in **`SYS1.AMODGEN(SYSEVENT)`**, and so are `IHAASCB`,
`IRAOUCB`, `IEFZB610` and the PPT generator `SGIEF0PT` (positive control: `WTO` reads in
MACLIB; `SYSEVENT` and `IHAASCB` both return `PDS member not found` there). 56 mnemonics;
**`DONTSWAP` = 41, `OKSWAP` = 42** (also `TRANSWAP` 14, `REQSWAP` 43); `&ENTRY=SVC` (default)
→ **`SVC 95`**, `ENTRY=BRANCH` → a `CVTOPTE` branch entry. **The macro documents no state
requirement in its 247 lines** — a fact about the macro, NOT an unestablished requirement: the
ancestor states it outright (`mvsevent.asm:89`, `SPKA 0  SYSEVENT requires key 0`) inside a
`testauth()` → `SUPERVISOR` → key-0 sequence, **and NSFS already holds all three**. SVC 95's
live `SVCTABLE` entry reads **type 1 with `svcapf` OFF**, so the FLIH turns nobody away and
any check lives **inside SRM**. **`ASCBNSWP` IS NOT THE LIVE INDICATOR — `OUCBNSW` IS**, and
this is the trap the round nearly fell into: `ASCBNSWP` reads *"PROGRAM IS NON SWAPPABLE OR
WILL RUN IN V=R REGION"* — a program attribute — while SRM's live status is `OUCBNSW` (BIT0 of
`OUCBSFL`). Only `*MASTER*` carries `ASCBNSWP`, but **JES2 and VTAM are non-swappable and show
it only in `OUCBNSW`**; a survey reading the ASCB bit alone reports them swappable and is
wrong. **On the stand: exactly three address spaces are non-swappable** — `*MASTER*`, `JES2`,
`NET` (VTAM) — **stable 12/12 across a 5.5-minute window**, all three also `OUCBASW` with
`OUCBNDS > 0`. **HTTPD is swappable**, and so are UFSD, FTPD and NSFS: **no project in this
ecosystem runs pinned**. **FTPD swapped out and back on 11 of 12 samples while idle** —
swapping is the normal condition here, not an exception, which is the counterweight to pinning
anything. **THE ECOSYSTEM'S ANSWER IS THIS PROJECT'S OWN ANCESTOR, and what transfers is the
MECHANISM ONLY:** `mvs38j-ip` issues `SYSEVENT DONTSWAP` at sysinit (`sysinit.c:67`) and
`OKSWAP` at termination (`xdone.c:51-54`) because SRM left it unavailable for ~25 minutes —
SRM making a long-running server unavailable for minutes, cured by DONTSWAP, is the precedent
and the whole of it. **His DIAGNOSIS does not transfer:** his trigger is `STIMER WAIT` from
**MVSDOZE, a governor against WTO buffer exhaustion**, not a device condition; he discounts the
CTCI MIH line himself (*"perhaps it's related, perhaps not (probably not)"*) and calls his SRM
conclusion *"merely my best guess"* — so "same device" carries no weight and inheriting a
disclaimed diagnosis would repeat a mistake already paid for. **Precedent cited, no code
copied** (ADR-0005); `libc370` has no SYSEVENT service, only the `cvtopte` field comment, so a
seam is new HLASM written fresh. **PHASE-1/PHASE-2 ASYMMETRY:** NSFS is `ac = 1`, calls
`clib_apf_setup` (SVC 244, `nsfsmain.c:237`) and enters `__super(PSWKEY0, …)` at **twelve**
sites in `nsfsx.c`; the Phase-1 `NSF` module has **no `ac`, no `clib_apf_setup`, no
`__super`** — **the mitigation is free exactly where the defect lives and not free anywhere
else.** **THE COST, WITH ITS FIELD:** `ASCBFMCT` ("ALLOCATED PAGE FRAME COUNT",
`IHAASCB` line 171, offset `X'98'`) = **39 frames / 156 KB for NSFS** against a **4096-frame**
machine (`MAINSIZE 16`, read from `conf/local.cnf` — the file the running `hercules -f`
actually names, checked against the process, not `custom.cnf`); idle and resident, so a
**floor**, not a working set (HTTPD's 180 resident vs 279 `OUCBWSS` is the gap). **PPT COST:**
no `SCHEDxx` in PARMLIB, `IEFSDPPT` is **not a member** of LPALIB/LINKLIB/NUCLEUS/SVCLIB (a
CSECT link-edited elsewhere), and the sysgen `&PGM` input adds CPU-affinity entries with
`DC X'00' SLOT FOR PROPERTIES` — **no attribute bits** — so a PPT change is a **USERMOD + IPL
for every installation**. **NONE OF THE THREE PREDICTIONS FIRES AS WRITTEN:** S1's first half
confirmed (mechanism exists, NSFS has more state than predicted) and its second half NOT
established; S2 not refuted, only unconfirmed; S3 partly fires, in the strongest form, but as
**precedent rather than an alternative** — it sharpens S1-vs-S2 instead of bypassing it.
**THE OPEN QUESTION IS THE WHOLE TRADE-OFF:** `PPTNSWP` reads *"to be **AUTHORIZED** to be
non-swappable"* and `OUCBASW` reads *"AUTHORIZED FOR DONTSWAP"*, so an authorization concept
exists — but the three ASes with `ASW` are **also exactly** the three with `NDS > 0`, so the
correlation cannot separate "granted at ATTACH from the PPT" from "set when the first DONTSWAP
was accepted", and **no discriminating case runs on this stand** (TCAM/GTF/IMS/JES3 absent).
The ancestor cannot settle it either — no `IEFSDPPT` appears in that repo, but a Turnkey
system's PPT would not be in that repo either way, so the absence is evidence about the tree
and not the machine. **One measurement settles it: issue `DONTSWAP` from NSFS and read
`OUCBNSW`/`OUCBNDS`/`OUCBASW` back** — a change, therefore not the probe's to make.
**RECOMMENDED: the self-issued route, gated on that measurement** — it costs an installation
nothing, where a PPT entry would turn a stack installable by copying a PROC and a load library
into one that requires modifying the operating system. **A CONTROL CAUGHT A REAL FAILURE:**
two offsets (`ASCBFMCT`, `OUCBWSS`) were in no prior round's proved set, so rather than submit
an IFOX00 job (64-0e exhausted the spool) the DSECT layouts were computed from the macro
sources and **gated on reproducing every offset a prior `CBOFF` job had proved** — `IRAOUCB`
17/17, `IHAASCB` **0/13 on first use**, because that member marks continued operands with a
trailing ` -` the parser did not strip. Without the gate the survey would have read a wrong
offset and reported a confident number. **INSTRUMENT TRAP: HTTPD is the instrument** (`/.dm`
runs inside it), so its own `ASCBFMCT` climbed 187 → 200 while being read — its frame count is
an upper bound, never a measurement; same shape as 64-0f's false CPU reading. **Does NOT
establish:** whether the PPT authorisation is required; that the running `IEFSDPPT` matches
`SGIEF0PT` (11/11 correspondence is consistent with it — the live table was **not** read);
that `DONTSWAP` would fix #64; NSFS's working set under load; anything about how SRM decides
to swap; or that pinning has no cost beyond storage. Host **2925 PASS / 0 FAIL** — a
no-regression check only. [[nsf370-64-3-0-noswap-survey]]
**64-3-1 — `SYSEVENT DONTSWAP`: the probe, and the mitigation — DONE, live-green, PR #77.
It MITIGATES #64 and does not fix it; #64 stays OPEN.** Not a milestone step; M5 stays in
progress. NSFS now issues **`SYSEVENT DONTSWAP` at init and `OKSWAP` at shutdown** (**ADR-0044**).
**STAGE A — the probe, because SYSEVENT/SVC 95 is a seam this project had never used.**
Operator-gated (`F NSFS,SWAP`), **never on the startup path**, so a wrong answer cannot break a
normal `S NSFS`; the verb rides the `g_statsextra` seam pattern (a handler pointer NULL until a
build registers one), so **Phase 1 is unchanged** — `F NSF,SWAP` draws `NSF808E` and the help
text does not grow. It had to run **inside NSFS's address space**: 64-3-0 could not separate
"`OUCBASW` granted at ATTACH from the PPT" from "set when a DONTSWAP is accepted" because the
only three ASes with `ASW` were the only three with `NDS > 0`. **THE READ-BACK IS THE PROOF AND
R15 IS NOT** — accepted / rejected / **silently ignored** are three outcomes and the third is
this project's standing failure class; SRM documents no return code for these codes (the
ancestor recorded it, `mvsevent.asm:15-16`), so R15 is reported, labelled unspecified, and
**never branched on**. Verdict from `OUCBNSW`/`OUCBNDS`/`OUCBASW` read through **one** path with
**one** identity assertion (`'OUCB'` eyecatcher **and** `OUCBASCB` pointing back at our ASCB —
the 64-0c trap one control block over) at every read point. **`OUCBNDS` is a COUNT: the release
is proven against the baseline VALUE, never against zero** — a probe that left NSFS pinned would
have changed the machine as a side effect of measuring it. **Live, twice byte-identical:
`NSW 00→80`, `NDS 0→1`, released clean.** **AND IT SETTLED 64-3-0's OPEN QUESTION: `ASW` stayed
CLEAR across an ACCEPTED DONTSWAP** (`AFL=X'48'` = `APG`+`JSR` throughout) — so ASW is **not**
set by an accepted DONTSWAP **and not** a precondition for one. **NSFS has no PPT entry and the
request took, so the PPT route is NOT REQUIRED and the self-issued route costs an installation
nothing** — S1 resolved by measurement, not correlation. Narrowly: proven only that the PPT entry
is not a precondition *for the request taking effect*, not that `ASW` has no other consequence.
**THE SEAM** (`asm/nsfsevt.asm`) is **derived, not copied** — as370 has no `SYSEVENT` macro
(mvsmacs/pdptop checked), so the two instructions the macro generates for the no-ASID/`ENTRY=SVC`
case are written out; codes re-derived live this round (`DONTSWAP` 41 line 91, `OKSWAP` 42 line
93, `SVC 95` line 209). **LEAF FORM, AND THE RULE IS THE SVC TYPE, NOT "ISSUES AN SVC":**
`NSFCIHLT` needs `SAVE=` because **SVC 33 is type 2**; **SVC 95 is type 1** (measured off the live
SVCTABLE in 64-3-0) so it saves into the FLIH's SVRB and never walks the caller's R13 chain —
which is also why `nsftmr_plat_arm` issues STIMER (SVC 47, type 1) as a leaf. **§3's sentence is
too broad as written and `NSFTMARM` has contradicted it since M0-5.** No `SPKA` in the seam:
`__super(PSWKEY0,…)` does `MODESET MODE=SUP` **and** `SPKA`. as370 listing checked
(`L R0,0(,R1)` = `5800 1000` base R1 not dropped to 0; `SVC 95` = `0A5F`), all 8 statements
present in order — **and that check was verified to discriminate: an over-long comment on the
`L` line made as370 merge the next statement and DROP THE `SVC 95` entirely.**
**STAGE B.** `DONTSWAP` right after `clib_apf_setup` (earliest possible, where the ancestor put
it); `OKSWAP` **after `nsfsx_stop()` and the device quiesce, before the ESTAE is deleted** —
those two steps must not be swapped out mid-flight, and a fault in the release should still have
recovery. **One call covers BOTH `nsfsx_stop` branches** (drained and retain) because it returns
either way and the teardown is linear: a pinned AS left pinned after `P NSFS` is the same debt
class as a retained anchor. **DESIGN PIN: a refusal WARNS AND CONTINUES** (`NSF852W`), never
refuses to start — the SVC steal refuses because that is a *correctness* failure; this is a
*latency* mitigation. **Phase 1 can never issue SYSEVENT, STRUCTURALLY** (`src/nsfswap.c`,
`asm/nsfsevt.asm` in the NSFS module source list only; Phase 1 has no `ac=1`, no
`clib_apf_setup`, no `__super`). **THE GATE DID NOT DISCRIMINATE, AND THAT IS THE ROUND'S MOST
IMPORTANT NEGATIVE RESULT.** With `DONTSWAP` reverted and NSFS verified swappable (`SFL=00` on
all 163 samples), **nine minutes of heavy non-vacuous load** (`SERVED=12997`,
`COLLISIONS=33538`, `EXHAUSTED=517`) produced **one** distinct `ASCBSTOR`, `OUCBSWC` never off
zero, `QFL` only `00`. **The control shows no swap transitions either, so the pinned arm's null
is uninterpretable.** **A prior round had already refuted the premise:** 64-0e measured NSFS
**resident 54/54 over 38 min** while NSFV swapped 12/16 — **NSFS does not swap on this stand
under any load this round can produce**, which is also why the stalls are rare. So the mitigation
rests on (1) **MEASURED** — DONTSWAP accepted/effective/released, five observations — and (2)
**REASONED, NOT MEASURED HERE** — MVS does not swap a non-swappable AS, which follows from what
`OUCBNSW` *means*. **The control for the pinned side remains 64-0f's stall**, on record, not
re-run. **THREE DEPLOYED STATES, EACH PROVEN POSITIVELY:** pinned (`NSF851I … NDS=1`); reverted
(`NSF851I` absent is *ambiguous alone*, so the check is the probe reporting **`NSW=N NDS=0`** on
a fresh STC — impossible on the pinned build); restored (`NSF851I` again, source
`git diff --quiet` identical to committed). The revert was verified as **exactly one change** by
a comment-stripped diff. **COST, LIKE-FOR-LIKE** (both arms 163 samples / 9 min, one sampler,
monotonic): the ONLY fields that differ are `SFL` and `NDS`; `ASCBFMCT` **peak 186 (744 KB) both
ways**, floor **56 pinned vs 46 swappable** — **pinning costs 10 frames / 40 KB more resident**
of 4096. **Pinning does NOT prevent page stealing** (186→56 while pinned); only the *swap* is
prevented — which corrects this round's own earlier note and makes 64-3-0's 39-frame idle figure
comparable rather than anomalous. **THREE FAULTS OF MINE IN THE RECORD:** a **bug found by
re-reading the code, not by a test** — `swap_set` returned on the `__super` failure path
**before** writing `*out`, so `NSF852W`, the very message the design pin exists to produce, would
have printed an uninitialised struct (never fired live because DONTSWAP was accepted); **arm 1
was contaminated** by two samplers writing one file (a `nohup` run I believed dead, plus one that
truncated the file underneath it) and is **SUPERSEDED, not caveated**, the re-run script now
killing strays and **asserting none survive**; and the sampler **took the password on `argv`**,
visible in `ps`, fixed to read the environment. **`NSF853I` IS NOT PROOF OF A RELEASE** —
`nsfswap_okswap` returns success whenever `OUCBNSW` ends *clear*, also true when nothing was
pinned, and it was observed live on the revert build. **Regression:** host **2934 PASS / 0 FAIL**
(TSTOPR 25→34, new assertions **verified to discriminate**); cross-build clean, alias scan **224
unique**; **NSFV round `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH`/`TSTXFW` 484 PASS CC 0 batch+TSO**;
**NSFS `TSTRQXC`/`TSTRQXF` 122 PASS CC 0 batch+TSO**; **`TSTRQXM` batch CC 0** with the host peer
verifying **9353 bytes byte-exact** and the batch run passing *"the first PARKED request to
complete"* (TSO FAIL by design, one-shot listener consumed, `errno 61`); **no dumps**.
**Round-order note:** the Stage-0 tests are clients of **NSFV**, so `P NSFS` alone leaves no SVC
router and they abend `SFEF` — the order is `P NSFS` → **`S NSFV`** → run → `P NSFV` → `S NSFS`.
**SORTED BY MODULE STATE, NO STALL HAS EVER BEEN REPRODUCED ON A RESET MODULE** (ADR-0044 §5a,
added at the countersign): all **nine** on record were on the **spinning** pre-64-1 module;
64-0f's one-in-four was on its deliberate spin revert build; on a **reset** module there have
been **none** — 64-1's Gate 2 (45 rounds, run after the reset), 64-0e (NSFV 543 356 requests,
NSFS 139 240, max inter-step gap 1 s), 64-3-0, and both arms of 64-3-1. **That is why the gate
had no control** — not a badly-run round but a phenomenon that does not occur on the current
module — **and the reason is concrete: 64-1's reset is one statement in `src/nsfsx.c`, which
64-3-1 does not touch**, while the revert removed the `DONTSWAP` call in `nsfsmain.c`, so the
control arm was *swappable but still reset* and varied the wrong axis. **The gate's premise was
a measurement lifted out of its configuration** — 64-0c's "reproduced twice within 90 seconds"
quoted without carrying that both were on the spinning module; **the second time in this
investigation**, so it is named as a recurring failure mode. **So 64-1 may already be the fix
for #64**, found from the far end (64-0f's F(i)) — **the evidence is ABSENCE across five rounds
and is LABELLED WEAK** (none designed to reproduce a one-in-four phenomenon; 64-1's own arm held
the shared condition ~0.8 % of its sampling). It makes DONTSWAP **defence in depth on top of a
probable fix** rather than the only thing between the stack and a twelve-minute outage; it does
not retire the mitigation (40 KB against an outage) and it does not close #64.
**THE FAILURE PATH WAS EXERCISED DELIBERATELY** (§8): the `__super` arm **never fires in normal
service**, which is the awkward shape in full — *a failure path that never runs is never tested,
and its absence is not even visible*. Induced **LIVE** (host was not an option without a new
shim for `<clibos.h>`: the existing `src/*_host.c` shims replace whole **asm modules**, not a
libc370 header — and the property under test is the *emitted message*), with the guard
short-circuited `if (1 || __super(...))` so the arm is taken **without calling `__super`**
(inverting the comparison instead would have entered supervisor key 0 and returned without
`__prob`, leaving the task authorised). Emitted: **`NSF852W NSFS REMAINS SWAPPABLE -- DONTSWAP
NOT ACCEPTED (NSW=N NDS=0) -- CONTINUING, SEE ISSUE #64`** then `NSF040I`/`NSF001I` — values
**defined** (the zeroed out-param), and **the pin holds live: it warned and CONTINUED**. Also
`NSF854W … OKSWAP NOT CONFIRMED` at shutdown. **And the message is TRUE, not merely
well-formed:** `F NSFS,SWAP` independently read `NSW=N ASW=N NDS=0` on that instance, and
`nsfswap_probe` calls `__super` **itself** rather than through `swap_set`, so it was unaffected
by the forced failure. Restored and re-proven `NSF851I … NDS=1`, source `git diff --quiet`
identical; re-run after the arm: NSFS **122 PASS**, NSFV **484 PASS**, host **2934 PASS**, no
dumps. **DESIGN PIN RATIFIED: a refusal warns and continues** — correctness failures refuse to
start (the SVC steal's posture, unchanged); a latency mitigation that cannot be applied is a
warning naming the condition.
**#64 STAYS OPEN — mitigated, not fixed, with the investigation deferred rather than closed —
and (e) STAYS BLOCKED, because the kickoff's condition was "unblocked only if the gate holds"
and it did not.** [[nsf370-64-3-1-dontswap]]
**M5-2c1 stage a (the caller identity, the APPS report, and the measurement that
BLOCKS the sweep) — DONE; stage b NOT STARTED, awaiting Mike.** The first sub-step of
M5-2c; **M5 stays in progress and no milestone flips.** c1 carries obligation #3 —
reclaim the sockets of an application that ends without TERMAPI. Reclaiming means
classifying its owner **DEAD**, and ADR-0040 never reaps an UNKNOWN, so one unmeasured
fact decides whether the feature can work at all. The round was cut in two on that, and
stage b did not start. **THE ANSWER IS `LIVE`, which was not one of the three outcomes
the kickoff anticipated** (DEAD / UNKNOWN / DEAD-after-delay): seven seconds after
JOB02807 reached OUTPUT CC 0000, and still LIVE at **T+6m31s** (11 polls, **28 slot
readings, every one LIVE**, `0 DEAD` in every summary). **The reason is that a batch job
has no address space of its own** — the recorded ASCB is the **INITIATOR's**, and the
initiator does not terminate when a job ends. **Proven live, not inferred: three
different jobs reported the SAME `ASCB=00FE7B58 ASID=0006`** (`TSTAPPDC` submitted after
`MBTTEST` had already reached OUTPUT). So the ASVT entry never goes AVAILABLE and
**`nsfreqx_classify` is not wrong** — it answers "did that address space end?", and for a
batch client the answer is no even though the application is gone. **The consequence is
sharper than "reclaims nothing": because the initiator is REUSED, a recorded identity
keeps answering LIVE while a DIFFERENT application is running there** — it does not merely
fail to expire, it stops denoting what it was recorded for. **The failure direction is the
safe one** (a false LIVE leaks a slot; it never tears down a healthy app's sockets — the
safe-side asymmetry holding under a condition nobody anticipated). **CANCEL is not a
second case** (JOB02809, `ABEND S222`, also LIVE — a cancelled job frees its initiator
exactly as a normal end does), and **the CLEAN control earns the other readings their
meaning** (TERMAPI released its slot, so "2 of 16 in use" is not a registry that never
empties). Every reading is **cross-checked against an identity the client WTOs before it
ends** (`__ascb(0)`, `ASCBASID` at ASCB+X'24'), so no line is about some other address
space. **Predictions recorded BEFORE the CLEAN/CANCEL arms ran** (`docs/measurements/
m5-2c1/predictions.md`) — all three fired. **NOT ESTABLISHED, and it is the question a
follow-up asks: the STC / TSO case.** An STC *is* its own address space and does
terminate — the case the classifier was built for and the one that matters for M6, since
**HTTPD and mvsMF are both STCs** — and it was not measured. **`TSTDEATH` does not cover
it either: its DEAD rows stage a SYNTHETIC identity** (`tstd_free_asid` scans for an ASID
already marked AVAILABLE), so **no test in this tree has ever watched a real address space
die.** **Delivered and durable:** the rename `SOCKCB.owner_ascb`→**`apptok`** (it had held
an app token since M3-2 while its name said ASCB); **the identity travels transport →
`nsfreq_dispatch_id` → `do_initapi` as a PARAMETER**, recorded at `CLAIMOK` from the
FLIH's R7 — so **`asm/nsfvsvc.asm` is untouched, the anchor layout does not move, and
`NSF_SIZE_ASSERT(NSFRQE, 64)` holds** (a request-supplied identity is what the guard must
never trust); **`F NSFS,APPS`** (`NSF814I`/`NSF815I`/`NSF816I`, in-use slots only, verdict
as a WORD) — read-only, and the instrument that produced the finding. **The red line is
enforced in ONE place**, `nsfreq_app_classify`: a zero caller ASCB (every Phase-1 slot)
answers `NO-ID` and **is never handed to the classifier** — asking "is address space 0
alive" has no true answer and both answers are bugs. **Phase 1 is unaffected**:
`nsfreq_dispatch(r)` ≡ `nsfreq_dispatch_id(r, 0, 0)` so all 40-odd direct-call sites are
unchanged, `src/nsfmain.c` registers neither seam, `F NSF,APPS` stays `NSF808E` and the
help text does not grow (pinned in TSTOPR against the *unregistered* verb). **Two
reclamation paths, one classifier** — the drain reaps CSA request slots at the transport,
this would reclaim app slots and sockets in the executive; they share `nsfreqx_classify`
and nothing else. Host **2934→2991 PASS / 0 FAIL**, and **all three new scenarios are
VERIFIED TO DISCRIMINATE** (dropping the identity stores fails 2; dropping the zero-ASCB
guard fails 6; an unregistered APPS is asserted inert). Cross-build clean (6 modules + 52
test modules, no warnings); alias scan **235 unique, all ≤ 8 chars**. **A SEPARATE
BLOCKING FINDING, reported not resolved: the locked rate limit has no clock.** "At most
once per 100 ticks, no timer" is unsatisfiable — since ADR-0034 *queue empty ⟺ STIMER
disarmed*, so with nothing armed `nsftmr_wake` is never called and no NSFTMR-derived tick
advances (64-1: one pass in 259 s). The failure case is not "nobody asks" but **"somebody
asks after an idle period"**, which is the normal case. Three options are laid out
(pass-count / `nsf_now()` with a documented constant / a new tick seam); **none chosen —
Mike's call.** Each LEAVE/CANCEL arm costs one app slot of 16; the round leaked three and
`P NSFS`/`S NSFS` reset it (**verified**: STC 1505 came up `0 OF 16 SLOTS IN USE`, no
`NSF054W`, `SVC 239 RESTORED`, stand left clean). `docs/measurements/m5-2c1/`.
**40-CHK (does the ADR-0040 guard protect anything for a batch client?) — DONE, live-green,
docs-only. It fixes nothing; the answer is NO.** Not a milestone step; **M5 stays in progress**
and c1 stage b is still with Mike. Stage a proved a batch job runs in a **reused initiator**, so
40-CHK asked the consequence: when a batch client dies with a request OUTSTANDING, does the guard
permit the reply POST? **G(i) fires, both halves measured.** Outstanding proven by the
**CONJUNCTION** (`req_state=1` **and** `NSF813I BUSY=1 BUSYSLOT=0 INFLIGHT=1`) — `PENDING` alone is
also the state of a request already in service, so it does not discriminate. Client killed
(`ABEND S222`, OUTPUT, gone from `D A,L`, **verified before anything else**); guard answers
`ASCB=00FE7330 ASID=0008` **LIVE**; a host connect completing the parked ACCEPT gives
**`served` 9→10, `req_state` 1→2 (DONE)**, `reaped 0`, no `NSF050I`/`NSF051W`, no abend — and
`served++`→`DONE`→`__xmpost` is one straight-line sequence, so **the POST was attempted into an
address space whose client task was dead.** **What it did: nothing observable** — the reply ECB at
`00A8B808` still reads `809DE5F0` (the dead task's WAIT bit + RB address); **the control arrived
free**, the next client took slot 1 which reads `reply_ecb=40000000 req_state=0` — a live client's
ECB is posted and its slot returns FREE, the dead one's is neither. **The cost is a PERMANENT LEAK,
not corruption:** `req_state` stuck at DONE (the party that releases a slot is its owner),
unrecoverable **by design** — `slot_action` is `ACT_NONE` for anything not PENDING and
`reap_ok(DONE, LIVE, trusted)` is 0; both correct given a LIVE verdict, **the verdict is the
defect**. Measured: `inflight` leaked at 1, **`collisions` 0→4** (every later claim scan walks the
dead slot and pays), the app slot leaked too; 64 such deaths exhaust the pool. **§2.3 discharged,
not fulfilled** — the kickoff's premise that *"the reply ECB lives in the client's private storage"*
is **WRONG** (`asm/nsfvsvc.asm:579-585` waits on `SLRECB` in the **CSA** anchor, corrected from
source BEFORE the run): the private region IS reused (a different job reported the same ASCB/ASID
**and byte-identical** `STACK=000D1348 HEAP=00094C68`) but the ECB is not in it, so the overlap
cannot arise. **G(ii) refuted** (nothing stopped the POST; nothing partners the guard); **G(iii)
refuted twice**. **TWO ISSUES FELL OUT, filed at Mike's direction, priority his:**
**#79 (certain, fires on EVERY abend, costs an IPL; **FIXED in PR #84**, below)** — `nsf_recover` calls `nsf_shutdown()` and
never `nsfsx_stop()`, so an abend leaves **SVC 239 stolen**, the router loaded, the anchor ACTIVE
and ~139 KB of CSA leaked; `S NSFS` then fails `NSF049E`→`SA0A`. **This contradicts CLAUDE.md §3
and `nsf_recover`'s own comment** ("a crash must never require a Hercules restart to clean up") —
it required one here. The refusal to re-steal is **correct and not the defect**. **#80 (suspected,
UNCONFIRMED)** — a cross-AS **receive** completes by writing `r->ubuf` = `slot->stage`, key-0
`SP=241` CSA, from the executive's **key 8**: the store M5-2b0 measured faulting `S0C4`. Every other
CSA write sits in a key window; the protocol op's does not. **Phase 1 unaffected** (same-space key-8
`ubuf`), and **no test covers a cross-AS data-returning receive** — TSTRQXM covers `send`/`connect`/
`close`, so "never worked" is consistent with everything green. **A round-hygiene failure of mine is
in the record:** I read `IEE341I NOT ACTIVE` after a stimulus as "the stimulus crashed it" when NSFS
had abended 36 s earlier for #79 and **the control never ran** — an absence that looks exactly like
its own result (§8.5); startup is now positively confirmed (`NSF041I`+`NSF001I`) before any
stimulus. **Still untested, the same gap stage a named:** an STC/TSO client *is* its own address
space and does terminate — no test in this tree has ever watched a real address space die. **Next
artifact is an ADR-0040 annotation naming the client class it does not cover, plus a decision — not
a patch.** Host **2991 PASS / 0 FAIL** unchanged. `docs/measurements/40-chk/`.
**COUNTERSIGNED (PR #81 merged); the annotation was REFRAMED at the countersign, and the
reframing is the part that carries.** The guard is **not, and never was, a dead-client
detector** — `src/nsfsx.c:316-317` states its purpose in the source: "`__xmpost` dereferences
the recorded ASCB, and the ASCB of an ended address space is reused SQA. **Compare the ASCB
ADDRESS only.**" It guards the POST against a **stale pointer**. For a batch client the ASCB is
genuinely live, so it returns the **correct answer to the question it asks**; what is dead is
the *task* inside a live address space, and **nothing in this system asks about that**. So the
record is a **correction of a reading, not a defect in the guard** — the first heading read "the
guard is inert for batch", which invites someone to repair the guard, and the gap is not there.
**And the ADR's open direction now carries its ceiling:** `__xmpost` is **`void`**
(`libc370/include/clibos.h:95`, implementation `src/clib/@@xmpost.c:5` — verified in source),
so **there is no return code the transport could learn from**; the only observable is the ECB
read-back, which exists **only when there is something to post**, so it can never serve **the
sweep**, whose subject is clients that ended with **nothing outstanding**. It could at best
recover 40-CHK's leaked slot — a client that died *with* a request in flight — and addresses
nothing in M5-2c1's. #80 stays open and unpatched and #64 stays open; **#79 was FIXED in PR #84
(below)** — it was open when this round reported.
**40-IDENT (what can a recorded identity actually distinguish?) — DONE, live-green, read-only,
no production code.** Not a milestone step; **M5 stays in progress** and c1 stage b is still with
Mike. **The answer: something changes, and it is not an identity** — kickoff predictions **I(ii)
and I(iii) both supported, I(i) not**. **Offline first, through 64-3-0's DSECT gate unmodified**
(`IRAOUCB` 17/17, `IHAASCB` 13/13, new `IHAGDA` 1/1 on the `CSAPQEP` 64-3-0 proved; the live
`IHAASCB` is byte-identical to 64-3-0's capture — the control on the fetch): `ASCBJBNI` (`X'AC'`)
and `ASCBJBNS` (`X'B0'`) are **`DS A` — POINTERS**, both IFOX00-**proved** not derived; the macro
**names the two client classes itself** (JBNI "FOR INITIATED PROGRAMS OR ZERO", JBNS "FOR
START/MOUNT/LOGON OR ZERO"); and there is **no `ASSB` and no `STOKEN`** anywhere in `IHAASCB`
(count 0 **with a positive control in the same grep**) — the architected per-instance identity of
later MVS **does not exist here**, which is most of I(ii) without touching the stand. (`ASCBFMCT`
is **`DS H`, a halfword** — a fullword read reports 589824 frames on a 4096-frame machine.)
**ARM 1, live:** a running initiator reads `JBNS='INIT'` **and** `JBNI=<the job's name>`, both
targets **COMMON** hence readable from NSFS; idle → `JBNI` **ZERO**. So the field tracks
residency exactly. **THE DISCRIMINATING CASE WAS RUN, NOT INFERRED, AND IT KILLS THE DIRECTION:**
the same JCL twice into the same initiator is **byte-identical** (same ASID, ASCB, `JBNI` pointer
`FF8F58`, same eight characters) — **a jobname identity is unsound and must not be built.** The
pointer is no fallback either: two further runs gave `FF9390` then `FF8F58`, so it **repeats
across different submissions and differs for the same name**, and that second direction is a
**false DEAD — the unsafe one**. **I(iii):** the per-submission identity (the JES job number)
needs `ASCBASXB → ASXBFTCB → TCBJSCB → SSIB`, and `ASCBASXB` is **PRIVATE and ALIASED — `9DF300`
reported by TEN address spaces** — unreachable from NSFS (ADR-0039) and, from the instrument,
worse than unreachable: `/.dm` would have returned **HTTPD's** ASXB looking entirely plausible.
Classifying the pointer against the GDA private window (`PASTRT`/`PASIZE`, gated) **before**
dereferencing is what prevented that. **ARM 2 — the test that never existed.** `jcl/TSTAPPDS.jcl`
→ `SYS2.PROCLIB(TSTAPPDS)` starts the **same** client program with `S` instead of submitting it,
so the arms differ **only** in how they start; **left installed on purpose** (c1 stage b's gate
and c2's `ORPHAN` retirement both need a real dying address space, and nothing else in this tree
produces one). An **alive control was taken first and required LIVE**, and it confirmed the class
split from the other side (STC: `JBNS=<stcname>`, `JBNI=0`). Then **DEAD within one second** for
**both** death modes — CANCEL and normal end — ASVT `00FF8D00 avail=False` → `80FDB048
avail=True`, **both** ADR-0040 DEAD rows firing, and through the guard's **own** arithmetic
`SLOT 7 ... DEAD` sitting beside six batch slots reading `LIVE` **in one report**. So the guard
**fires correctly for an STC — inside a window the next paragraph measures and does not bound**;
that is not a green light for the sweep. **THE DEAD VERDICT IS NOT STABLE, AND NOBODY ANTICIPATED
IT:** both STC runs got the **same ASCB *and* ASID** (MVS reused the ASID and the ASCB block at
the identical address), and starting a third STC flipped the two provably-dead clients' slots
**back to LIVE** — `2 DEAD` → `0 DEAD`, **not reaped, reclassified**. **ADR-0040's ASID-reuse row
cannot catch this**, because it compares the ASCB *address* and the address was reused unchanged.
**So a DEAD verdict states what occupies that ASID RIGHT NOW, not what happened to the recorded
client** — correct only inside the window before the next occupant, **here under a minute**, and
**this stand is the FAST end** (three initiators, near-empty STC ASID range), so a busier system
gives a *longer* window: a sweep would look reliable under test and fail in production. **A sweep
is racing a reuse window it does not control** — a far harder constraint on c1 stage b than the
rate limit stage a was worried about. **The unifying result: batch identities NEVER die; STC
identities die and are RESURRECTED by reuse** — either way a recorded `(ASCB, ASID)` stops
denoting what it was recorded for, and the failure direction stays the safe one throughout.
**Two instrument failures are in the record rather than smoothed over:** a 576-sample all-IDLE
null and a 38-pass zero-hit sweep contradicted an earlier positive reading, so the instrument was
put under a **positive control** — the long job re-submitted, **seen in 32 of 33 passes** — which
proved the instrument sound and the nulls real (short jobs have sub-sample initiator residency);
without it the tidy, publishable and **wrong** conclusion was "the field is not populated for
short jobs". And one **non-printable** jobname sighting **could not be reproduced in 707
tightly-sampled transition samples** (likeliest cause: the tool reads the ASCB and the target in
two non-atomic `/.dm` round-trips), so it is recorded as an observation, not a finding. **§3.1's
layering rule is recorded with the findings and was followed by the probe itself:** `ufsd#asv.c`'s
hazard is an ASCB that is **gone**, ours is **alive hosting a different job**, so a candidate
field is read **only after** the ASVT-membership check establishes the ASCB is real. **Does NOT
establish:** any design; how long the reuse window is in general (**one** measurement); that the
non-printable transient is real; whether `JBNI` survives a swap cycle; **TSO**, a third class,
unmeasured; anything about #64/#79/#80. **Housekeeping:** the arms leaked 10 app slots of 16
(that is what the arms are), `P NSFS`/`S NSFS` reset the registry **verified** at `0 OF 16`; the
shutdown **DRAINED** (`NSF043I`/`NSF011I`/`IEF404I`, **no `NSF054W`**) and the restart reported
`LARGEST FREE BLOCK NOW 1073152`, **identical to STC 1534's own startup line at 03.52.18 before
the round** — no CSA debt, no IPL owed; the arms used `HANG`/`LEAVE`/`CLEAN` and **never**
`PARK`/`PARKA` precisely because a parked client leaks `inflight` and forces the retain branch.
**Zero dumps**, stand left as found. Host **2991 PASS / 0 FAIL** unchanged — a no-regression check
only. `docs/measurements/40-ident/`. [[nsf370-40-ident-identity-reuse]]
**#79 (an abend must not cost an IPL) — FIXED, live-green, PR #84. Not a milestone step; M5
stays in progress and nothing flips**; c1 stage b is still with Mike, and #80/#64 are untouched.
**The finding was a class, not a missing call:** every resource Phase 2 acquired was added to the
clean teardown and to **nothing else** — Phase 1 never noticed, because `nsf_shutdown()` was
COMPLETE when it was written. `docs/measurements/m5-79/audit.md` enumerates all **13** and sorts
them by the only line that matters: **AS-scoped** (MVS reclaims it at address-space termination —
recovery need not, and in a damaged environment should not, touch it: the private region, the SVC 99
device allocations, the I/O subtasks, the OUCB and hence the DONTSWAP) versus **system-scoped**
(common storage or the nucleus — **nobody reclaims it but us**). Exactly **three** are
system-scoped: the SVC slot, the router module, the CSA anchor. `nsfsx_recover_quiesce`
(`NSFSXRQ`) restores the **slot** and clears `ANCHOR_ACTIVE` + `server_ecb_ptr`; it does **NOT**
drain (a 10 s polling loop in a damaged environment) and does **NOT** free or unload — the M5-2b3
rule stands unchanged, so **recovery takes the RETAIN posture unconditionally**, and it runs
**BEFORE** `nsf_shutdown()` because `mm_shutdown()` releases every pool region and the one action
whose absence costs an IPL belongs ahead of every later step that can fault. **Two things had to be
fixed to make the gate possible at all:** `nsfsx_svc_restore` returned **silently** when `__super`
failed (§8.5 in pure form — it made "demonstrate the `__super`-fails posture" *unsatisfiable*), so
the table write is factored into `nsfsx_svc_restore_locked()` and **both callers share ONE
encoding** while the countersigned clean path stays byte-for-byte; and **`__prob` MODESETs to
problem state UNCONDITIONALLY** where `__super` skips the MODESET when already supervisor, so the
ordinary pair would drop an exit *entered* supervisor into problem state on the way back to RTM —
`__issup()` is captured at entry and the task returned to what it found. **`OKSWAP` deliberately
NOT added:** `nsfswap_read` reaches the OUCB **through the ASCB**, which is freed with the address
space, so there is nothing left to release. **THE EXIT'S STATE IS MEASURED, NOT ASSUMED:** `NSF902I
RECOVERY ENVIRONMENT: SUP=N AUTH=Y`, identical on **five** abends across **three** modules — an
ESTAE exit here is entered **problem state, APF-authorised**, which is what makes the slot restore
possible and the `__prob` return leg correct (the supervisor-entry branch is **unexercised** and
kept, because it costs one compare and the failure it prevents is silent). **VALIDATED LIVE on
MVSCE, four arms, exactly one assertion moving:** **B** (fix + a forced `S0C4`) → `NSF903I` →
`S NSFS` **SUCCEEDS**, `NSF042I SVC 239 STOLEN (EP 00A8B248)` **on the same IPL** — the line #79
says cannot happen; **B2** (same, injected **inside `evt_mainloop`**, #79's own lifecycle point) →
same; **C** (**fix reverted, one axis varied**, diff verified) → no `NSF903I` → **FAILS**
`NSF049E SVC 239 IN USE -- NOT STOLEN`; **D** (**forced `__super` failure**, the 64-3-1 `1 ||`
short-circuit so `__super` is never called and the slot is *genuinely* not restored) → **`NSF904E
... RC=2 -- NSFS CANNOT RESTART, AN IPL IS REQUIRED`** then **`NSF901I`** — the **warn-and-continue
pin holds** on this path too — then the **ORIGINAL** `S0C4` percolates, and the next start's
`NSF049E` confirms the failure was real and not a message rehearsal. **Deploy-took-effect is
positive in BOTH directions** (`NSF903I` present ⇒ fixed module; absent *together with* `NSF049E` ⇒
reverted), so no arm rests on an absence. **THE STRONGEST EVIDENCE WAS FREE, and the technique is
worth keeping: a retained CSA anchor is readable through `/.dm` AFTER its address space is gone.**
All four anchors dumped post-mortem — the three quiesced at `flags=00000000` /
`server_ecb_ptr=00000000`, the reverted one at **`80000000`** / **`000BE0D4`**, which is precisely
the address that STC's own `NSF041I` published: **a live pointer into the private storage of a dead
address space, left standing**. Everything else identical across all four (`NSFVANCR`, `version=3`,
`nslots=0x40`), so the comparison isolates exactly the two words recovery writes. **Row 6 confirmed
live from a restart that was happening anyway:** every start after an abend read `NSF851I ... NDS=1`,
never `2` — nothing carries over, so `OKSWAP` at recovery would be a no-op. **A SECOND DEFECT, FOUND
BY THE GATE AND NOT CAUSED BY IT — issue #83, filed not patched:** recovery never reaches `NSF901I`
on a stack with devices up, because `nsf_shutdown()` takes a second abend — `A0A`, the SVC 10
FREEMAIN family (`mm_shutdown()` frees its regions with `free()`), `IEA700I` operands byte-identical
across all five firings. Narrowed by a **controlled arm IPL #2 handed over for free** (the CTCI came
back offline, so **one binary** ran with **one variable**): devices **up** ×3 → `A0A`, devices
**down** ×1 → **no `A0A`, `NSF901I` reached**. That **excludes `NSF902I`** (it ran in the no-`A0A`
arm) and excludes the key window *more strongly* than arm C did, which only showed the `A0A`
surviving the quiesce's **removal**. **Mechanism = audit row 7:** recovery does not quiesce devices,
so `free()` runs while two ATTACHed CTCI I/O subtasks are live on a libc370 heap that is **not
reentrant across subtasks**; the clean path cannot hit it because `dev_foreach(nsf_quiesce_device)`
runs first. **Necessary but NOT sufficient** — 40-CHK's STC 1505 abended with devices up and reached
`NSF901I` — **which is evidence FOR a race, not against one**. **It costs nothing that matters** (the
pool regions are AS-scoped, and the `A0A` lands **after** `NSF903I`, which is why arm B restarts in
spite of it), and **the likely fix runs OPPOSITE to "quiesce devices in recovery"** — `dev_shutdown`
does OPEN/CLOSE/EXCP and joins subtasks, exactly the blocking work an exit must not attempt — and
toward **not calling `mm_shutdown()` in an exit at all**, since §1 already classifies those regions
as AS-scoped. So **audit §5's "rows 7 and 8, named and deliberately not fixed here" carries a
correction**: still no leak, but no longer merely cosmetic. **Regression:** host **2991 PASS / 0
FAIL** — a **no-regression check only**, since both changed files are MVS-only; cross-build clean (6
modules + 52 test modules); alias scan **236 unique**, one new (`NSFSXRQ`); `TSTRQXC`/`TSTRQXF`
**122 PASS CC 0 batch+TSO**; the NSFV round `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH`/`TSTXFW` **484
PASS CC 0 batch+TSO** (`TSTMVCD` excluded, #53); `S NSFS`/`P NSFS` clean both ways with **`NSF043I
SVC 239 RESTORED`** — the no-regression check that matters most for the refactor, the clean path
being the already-countersigned caller — **no `NSF054W`, zero dumps**. **`TSTRQXM` NOT RUN**, and
the reason is environmental, not a skip: Hercules failed to create the TUN at startup (`HHC00138E
... Interrupted system call` → **`HHC01463E 0:0501 device initialization failed`**, `devlist`
**empty**), MVS IPLed without 0500/0501, and 3.8j has no dynamic I/O reconfiguration — re-`attach`
brings `tun0` up and `devlist` clean but MVS still answers `IEE025I UNIT 500 HAS NO LOGICAL PATHS`;
only a full Hercules restart recovers it, and **`devinit` must not be used to force it** (it opens a
SECOND `tun` with the same address and a competing route). It exercises the cross-AS **data** path,
which this change does not touch. **The temporary abend injections are NOT in the merged diff and
were never committed.** **Gate-cost lesson for planning:** any arm that must FAIL to restart leaves
`SVC 239` stolen and costs an IPL, and **arm D must run BEFORE the revert arm** — it needs a clean
slot to start on. Getting that order wrong cost a second IPL. **ADR-0040 also gains its SECOND
annotation in this PR** (40-IDENT arm 2, above): a `DEAD` verdict is **transient** — both STC runs
got the same ASCB *and* ASID and a third start flipped two provably dead clients back to LIVE,
**reclassified, not reaped** — so `(ASCB, ASID)` is an **address, not an identity**, failing in both
directions at two scales. **40-IDENT §6's test-vs-production direction is CORRECTED there:** a sweep
reclaims only what it classifies DEAD and a slot is classifiable DEAD only *inside* the window, so
fast reuse means the sweep **misses more** — **this stand is the PESSIMISTIC case** and a hit rate
measured here is a **floor, not a ceiling**. The premise "a busier system gives a longer window" was
**never measured** and the one datum points the other way (the free entry read `80FDB048`, a
**free-chain** pointer; under LIFO the just-freed ASID returns first regardless of chain length —
what arm 2 saw), so the magnitude is unbounded in both directions and the **discriminator is named**
(free-chain discipline, unmeasured). The **safe-side asymmetry survives and is stated explicitly** —
reuse can only convert DEAD → LIVE, never LIVE → DEAD, since an ASID is unique among live address
spaces — and the window only means anything **relative to the sweep period**, the same knob c1 stage
a stopped on. `docs/measurements/m5-79/`. **80-CHK (does a data-returning cross-AS receive store into key-0 CSA from key 8?) —
DONE, live-green, docs + one opt-in probe. It fixes nothing; #80 is CONFIRMED and stays
OPEN.** Not a milestone step; **M5 stays in progress**, c1 stage b is still with Mike, and
#64/#83 are untouched. **The answer is yes, and the fix is a design decision with ADR
weight that this round deliberately did not build.** §0 first, because the previous round
could not run the data path at all: `TSTRQXM` **batch CC 0** with the host peer verifying
**9353 bytes byte-exact** (TSO FAIL by design — the one-shot listener consumed by the
batch run, `errno 61` on CONNECT + its dependent CLOSE, checked not assumed). **The chain,
re-derived from source:** `nsfreqx_dispatch_in` sets `priv->ubuf = slot->stage` — CSA,
`SP=241`, **key 0** — and `nsfsx_drain` dispatches **outside** the `__super` window by
design, so a protocol op that *writes* its result performs a key-8 store into key-0
storage. The named store is `src/nsfudp.c:200` `buf_copyout(bpay, r->ubuf, want)` →
`src/nsfbuf.c:285` `memcpy(d + total, b->data, take)` (TCP reaches the same call from
`src/nsftcp.c:628/639`). A grep for `__super` / `__prob` / `SPKA` / `PSWKEY0` across the
protocol layer returns **NOTHING** — the load-bearing negative: this is the ONE CSA write outside a
key window, and it is there because that code is *supposed* to know nothing about keys.
**Why it never showed:** CSA is key 0 and **not fetch-protected**, so a key-8 *fetch*
succeeds where a *store* faults (M5-2b0) — every cross-AS path exercised to date READS
`ubuf` (TSTRQXM's 9353 bytes are all sends), and **no test in this tree had ever driven a
cross-AS receive that returns data**. Not regressed; never worked. **THE CONTROL IS THE
ROUND'S DESIGN, because "NSFS abended S0C4" alone does not separate K(i) from K(iii):**
`udp_complete_recv` guards its copy with `r->ubuf != NULL && r->ulen > 0`, and
`buf_copyout`'s loop does not run when `n == 0` — so a **zero-length datagram** crosses
identically, calls the identical function on the identical request, and elides **only the
memcpy**. One line apart, one code path. Live (`test/mvs/tstrqxr.c` + `samples/host/
recvkey_peer.py`, unauthorised client): `ZERO-BYTE RECV RETURNED n=0` **present**,
`ARM -- DATA RECV RETURNED` **absent**, `IEF450I NSFS NSFS - ABEND S0C4 U0000` —
**reproduced twice**, the second time on the **final committed binary** so the artifact and
the evidence are the same thing. **The retained anchor was read back** (`NSF903I` retains
CSA; a retained anchor is readable through `/.dm` after its AS is gone — the m5-79
technique, instrument validated first against `NSF813I`/`NSF041I` field for field):
`served=6` **exactly** the six requests that completed, slot `req_state` **PENDING** (not
DONE — the write-out fault's shape, so the dispatch fault is distinguishable from it),
`reply_ecb=809DE5F0` (client parked, never POSTed), `inflight` leaked at 1, and
**`xlen=512`** — which is what forecloses "a silent zero-length no-op masquerading as
K(ii)": the length crossed and the store ran with `n > 0`. The peer log shows both replies
sent, triggers decoding as EBCDIC `R0`/`R1`, so the arm's datagram genuinely reached the
wire. **The client HANGS** (parked on `SLRECB`; recovery does not nudge parked clients) and
the `S222` cancel takes buffered SYSPRINT with it — `0 PASS, 0 FAIL` in the matrix — which
is why every step is a `wtof` marker and **the console markers ARE the result** (the M4-5
lesson, applied in advance rather than learned again). **#79 VALIDATED IN THE FIELD, twice,
on a NATURAL abend** (its own gate used an *injected* one): `NSF900E`→`NSF902I SUP=N
AUTH=Y`→`NSF903I`→`NSF901I`, and **`S NSFS` succeeded on the same IPL both times** with
`NSF042I SVC 239 STOLEN` on a **different anchor and a different router EP** each time —
the evidence the module was retained. **#83's `A0A` did NOT fire either time with devices
UP**, `NSF901I` reached — corroborating the m5-79 reading that devices-up is **necessary
but not sufficient**; a free data point about #83, not a finding, since nothing here was
designed to test it. **The probe is OPT-IN and the default is inert**: it abends NSFS and
its receives block with no timeout, so a bare run does nothing and returns **CC 20**
(`XR_CC_GATE_SKIPPED`, the TSTXFW/TSTRQXF idiom — "did not run" can never read as
"passed", §8.5), the arm needing `PARM='ARM'` via `jcl/TSTRQXR.jcl`. **CC 20 alone would not
have shown the guard fires for the intended REASON** (identical to `argc` never arriving
under `crt1`, or a PARM shape the compare misses — §8.5 aimed at my own guard), so the
not-run branch reports what it saw: **`argc=1 argv1=<none>`**, measured batch+TSO — which
also establishes that `argc`/`argv` do arrive under `crt1`. Both directions are therefore
measured (the other by JOB02864 having run the arm at all), corroborated from the STC side
by the untouched instance reporting `SERVED=0`. The arm ran on the binary **before** that
marker; the diff is **8 lines confined to the not-run branch**, so the arm's path is
untouched — the M5-2c0 standard. Like `TSTMVCD`, it is **excluded from a round's
regression set** (noted in `project.toml`): inert by construction, but a CC 20 still reads
FAIL in the matrix. **ADR-0041 gains a FOURTH write-out category** and it is different in kind:
1–3 are stores the transport makes and can bracket where it stands; **4 is made by code
this ADR deliberately kept ignorant of the boundary**, so the fix cannot be another `SPKA`
pair in `MOVEOUT` — a key window around the completion copy, or a key-8 landing area with a
keyed move, and choosing is Mike's. **DOES NOT ESTABLISH:** the **inline** (rxq-dequeue)
shape or **TCP** live — both reach the identical instruction *by construction*
(`udp_complete_recv`'s own header says it is shared by both paths; `dispatch_in` rewrites
`ubuf` for any crossing request regardless of protocol) but were **reasoned, not run**, and
TCP is what M6's HTTPD/mvsMF would use; whether the faulting `MVC` suppresses or
terminates; or any recovery from the dangling state (still the open M5-2 item ADR-0039
names). **Red lines held** — no key window added, no landing area, no `ubuf` rewrite, no
existing window widened, `asm/nsfvsvc.asm` untouched, anchor layout unmoved, `ANCVERNO` 3,
**NSFRQE frozen at 64 B**. **Cost:** each arm retains its anchor + router until IPL,
**139264 bytes measured identically both times** (1073152→933888→794624). **Zero dumps**
(`IEA995I` 0 against `IEF450I` 4 as the positive control — itself the right number: 2 NSFS
`S0C4` + 2 client `S222`). Host **2991 PASS / 0 FAIL** unchanged — a no-regression check
only. `docs/measurements/80-chk/`.
[[nsf370-m5-79-recovery-teardown]]
[[nsf370-a0a-recovery-device-subtasks]]
[[nsf370-m5-stage0a-prime-status]] [[nsf370-m5-stage0b-status]] [[nsf370-m5-stage0c-status]]
[[nsf370-m5-2b3-slot-pool]] [[nsf370-m5-2b4-contention]] [[nsf370-64-1-wake-ecb-reset]] |
| **M6** | *(stretch)* HTTPD + mvsMF on NSF; DNS; LCS + ARP | **Project success:** HTTPD & mvsMF run unchanged (relink) on TK4-/TK5 | ☐ Planned |

Critical path: **M0-1** (MBT project + build) and **M0-2** (NSFQUE/NSFMM);
the rest of M0 parallelizes. M5 carries the only real schedule risk and is
isolated so M0–M4 already deliver a usable in-process stack.

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
4. **On-MVS validation** via `make test-mvs` at milestone boundaries and
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
   - **`make deploy` failing mid-chain**, after which the run silently tests the
     previous module (§5 has the signature and the tell);
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
   chapter and add/append an ADR in the same change. Update §7 status here.
8. **Ask before changing an Issue's STATE, not just before creating one:**
   `gh issue close` / `reopen` / `edit` join `create` under the root
   CLAUDE.md's confirm-first rule, because the audit trail cannot tell us
   apart — every action is `mgrossmann` — so a state change nobody intended
   is unattributable afterwards and reads as a decision (issue #64 was
   closed `COMPLETED` while three places in the tree carried it as open).
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
| NSFDEV / NSFCTCI / NSFLCS / NSFHOST | Devices & drivers (NSFDEV table + DEVOPS + DEVIO seam + NSFHOST host driver, M1-2; CTCI top half `asm/nsfctcio.asm` (per-scb save areas) + SVC 99 seam `src/nsfctci.c` M1-3; codec `src/nsfctcif.c` + bottom half `src/nsfctcib.c` with the read/write **I/O subtasks** over the `nsfthr` seam (`src/nsfthr.c` / `src/nsfthr_host.c`) M1-4, ADR-0022/0023; host shims `src/nsfctcio_host.c`/`src/nsfctci_host.c`) | 9 | 200–299 |
| NSFSOC / NSFREQ | Sockets / Request mgr — socket table + SOCKCB + `(gen<<16)\|id` descriptor (slot-owned generation) + `PROTOPS` dispatch + parked-request pattern + `soc_destroy` teardown checklist + `soc_foreach` (`src/nsfsoc.c`, M3-1/M3-2); the `NSFRQE` phase-boundary contract + `RQ_*`/`RQ_F_NONBLOCK`/`NSF_E*` (`include/nsfreq.h`, **FROZEN at M3-2**; `apptok` named out of reserved). NSFREQ transport + fn dispatcher + app registry (`src/nsfreq.c`, M3-2): request queue (NSFXQ) + `requestECB` (wired via `evt_set_request`, reset-before-drain + double-check, ADR-0022), `nsfreq_submit`/`_wait`/`_call`/`_dispatch`/`_drain`/`_pending`/`_register_proto`. `soc_complete`/completion POST via `nsfthr_post` (same-AS SVC 2). UDP ops (M3-3, `src/nsfudp.c`) register the UDP PROTOPS via `nsfreq_register_proto(17,…)`; NSFEZA = M3-4 | 10 | 600–699 |
| NSFIP / NSFICM | IPv4 / ICMP — input validate/demux + output build/route + 16-entry routing table (`src/nsfip.c`, M2-2); ICMP echo responder in-place single-owner (`src/nsficmp.c`, M2-3); shared RFC 1071 checksum over a PBUF chain (`src/nsfcksum.c`, M2-1). ADR-0024; byte-wise big-endian, addresses UINT/octet-1-MSB | 11 | 300–399 |
| NSFUDP | UDP — UDPPCB + bind/demux (specific laddr beats ANY) + `nsfudp_input` (checksum-verify, RFC 768 zero-cksum both ways, port-unreachable trigger) + RQ_SENDTO/RQ_RECVFROM + `UDPADDR` rxq record + real PROTOPS + `nsfudp_reserve`/`_init`/`_protops` (`src/nsfudp.c`, M3-3, ADR-0028). Pseudo-header via `in_cksum_partial`/`_fold` seed (no overlay); IP demux via `nsfip_register_proto` (keeps NSFUDP out of the NSF module). NSFEZA = M3-4 | 12 | 400–499 |
| NSFTCP | TCP | 13 | 500–599 |
| NSFCFG | Configuration | 14 | 700–799 |
| NSFEZA | EZASOKET API | 15 | 600–699 |
| NSFOPR | Operator interface (dispatcher + CIB seam; M0-8) | 5 / 17 | 800–899 |
| NSFMSG | WTO message seam (libc370 `wto`; M0-8) | 5 / 17 | — |
| NSFSTC | STC startup + NSFCFG→init wiring (M0-8) | 5 / 14 | 000–099 |
| NSFFMT | Safe formatting seam (`nsf_vsnprintf`/`nsf_snprintf`; libc370 truncation fix, ADR-0026, issue #25) | — | — |
| (recovery) | ESTAE via libc370 `__estae` + C `nsf_recover` (ADR-0018; no NSFESTAE CSECT) | 17 | 900–999 |
