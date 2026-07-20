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
  instruction-line comments short and within column 71; put long rationale in a
  leading `*` comment block (those are full-width, whole-line comments and are
  safe).
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
| **M5** | Phase 2: `NSFS` subsystem + cross-memory + TCP hardening + docs | 2 address spaces share one stack; stress passes; docs complete | ☐ Planned |
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
5. **Definition of Done** (§7) must hold, including the leak gate.
6. **Keep docs honest:** when a decision changes, update the affected spec
   chapter and add/append an ADR in the same change. Update §7 status here.
7. **English** for all code comments, commit messages, and docs.

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
