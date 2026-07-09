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
- The `FUNHEAD` entry name IS the 8-char `asm()` alias, character for character.
  Address static data by **explicit displacement** `LABEL-entry(,R12)` (or a
  register), never a bare-label `USING` (as370 drops those to base 0 — the S102
  class). Keep `CS`/`LM` (RS-format) operands `D(B)`, never `D(,B)` (#5). Keep
  every statement inside **column 71** (as370 reads column 72 as a continuation
  flag and silently merges the next line).
- **Exception:** a routine the OS invokes as an *exit* (not called from C) is not
  a C callee and does not get `FUNHEAD` — e.g. `NSFTMEXP`, the STIMER exit.
- **Reviewer checklist (assembler):** a new C-callable routine uses
  `FUNHEAD`/`FUNEXIT`, not hand-rolled; its entry name matches its `asm()` alias;
  data addressed by explicit displacement; `CS`/`LM` stay `D(B)`; nothing past
  column 71. A green host build and a clean link are **not** evidence — this
  failure is MVS-runtime only (issue #8).

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
├── asm/              HLASM: nsfctci.asm nsfxq.asm nsfstim.asm nsftime.asm
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
| **M1** | CTCI driver (HLASM top / C bottom) + NSFDEV + NSFHOST | ping → hexdump in trace; crafted packet seen in host `tcpdump` | ◧ **In progress** — **M1-1 done** (CTCI wire format verified byte-exact vs Hercules `ctc_ctci.c`, written into spec §9.3 as normative: 3088 read/write pair, `CTCIHDR`/`CTCISEG`, big-endian). **M1-2 done** (device abstraction + host driver): `include/nsfdev.h`/`src/nsfdev.c` — the `DEVOPS` contract + fixed device table (`dev_register`/`dev_find`/`dev_find_cuu`/`dev_by_index`/`dev_foreach`/`dev_start`/`dev_send`/`dev_shutdown`), `NETDEV` 64 B (`NSF_SIZE_ASSERT`), `send`-ownership rule enforced. The executive loop stays **driver-agnostic** (never names HOST/CTCI): `NSFDEV` registers three hooks via `evt_set_devices` (mirroring `evt_set_operator`) — `nsfdev_collect_ecbs` (device ECBs→ECBLIST), `nsfdev_poll_input` (drain each `doneq`→`EV_PACKET_RECEIVED`, before dispatch, lost-wakeup-safe ECB clear, drop+count on EVT exhaustion), `nsfdev_kick_output` (§5.3 step 5); `nsfevt_wake` kicks output for a send from outside a loop pass. **NSFHOST** (`src/nsfhost.c`, host-only): `DEVOPS` impl whose inbound path is the **host analog of the CTCI I/O-completion exit** — a pthread reader thread `xq_push`es a received PBUF onto `doneq` + POSTs the device ECB, so the `doneq→EV_PACKET_RECEIVED` handoff is validated across a real thread boundary (M1-3 swaps only the producer). Default in-memory **loopback** (copy-free relay; NSFMM touched only on the executive task → no pool race), optional **TUN** (`-DNSFHOST_TUN`), **PCAP** reserved. Cross-build discipline: `nsfhost.c` is host-only, the MVS build compiles the NULL-ops placeholder `src/nsfhost_plat.c` (no host driver on MVS — use CTCI/LCS) swapped via `[host].replace`, so `test/tstdev.c` (portable; pthread lives in nsfhost.c) still cross-links and skips trivially where `nsfhost_ops()` is NULL. Host **408→488** (TSTDEV 80: send→receive cycle, in-order delivery, bounded `sendq`, DOWN-device reject, leak gate; 80/80 stress-stable); `-Wall -Wextra -Werror` clean (host + cc370); NSF module + all 16 test modules cross-link clean, alias scan clean (unique `NSFD*`/`NSFH*`/`NSFEVDEV`/`NSFEVWK`). ADR-0018/spec §9.2/§9.4/§19 updated. **M1-3 (CTCI HLASM top half: EXCP READ/WRITE CCW, I/O exit → xq_push+POST) next.** |
| **M2** | IPv4 in/out + routing + ICMP echo/errors + checksum | `ping <mvs-ip>` sustained, 0 loss on loopback link | ☐ Planned |
| **M3** | Sockets + NSFRQE + UDP + EZASOKET (M3 set) | UDP echo via EZASOKET from host; leak-free. **`NSFRQE` freezes here.** | ☐ Planned |
| **M4** | TCP (state machine, data path, rexmit) + EZASOKET (M4 set) + loss harness | telnet TCP echo, clean FIN, survives 5% loss; TIME_WAIT reclaim shown | ☐ Planned |
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
| NSFDEV / NSFCTCI / NSFLCS / NSFHOST | Devices & drivers (NSFDEV table + DEVOPS + NSFHOST host driver, M1-2; CTCI M1-3/4) | 9 | 200–299 |
| NSFSOC / NSFREQ | Sockets / Request mgr | 10 | 600–699 |
| NSFIP / NSFICM | IPv4 / ICMP | 11 | 300–399 |
| NSFUDP | UDP | 12 | 400–499 |
| NSFTCP | TCP | 13 | 500–599 |
| NSFCFG | Configuration | 14 | 700–799 |
| NSFEZA | EZASOKET API | 15 | 600–699 |
| NSFOPR | Operator interface (dispatcher + CIB seam; M0-8) | 5 / 17 | 800–899 |
| NSFMSG | WTO message seam (libc370 `wto`; M0-8) | 5 / 17 | — |
| NSFSTC | STC startup + NSFCFG→init wiring (M0-8) | 5 / 14 | 000–099 |
| (recovery) | ESTAE via libc370 `__estae` + C `nsf_recover` (ADR-0018; no NSFESTAE CSECT) | 17 | 900–999 |
