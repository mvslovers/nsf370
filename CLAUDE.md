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
  operands `D(B)`, never `D(,B)` (#5). Keep every statement inside **column 71**
  (as370 reads column 72 as a continuation flag and silently merges the next
  line).
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
  is the device-transmit counter — it lags `NSFIP out`/`NSFICM outecho` by the
  frames still in the write pipeline (a stalled last-of-burst write, [[nsf370-ctci-write-tail-stall]]).
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
| **M2** | IPv4 in/out + routing + ICMP echo/errors + checksum | `ping <mvs-ip>` sustained, 0 loss on loopback link | ◐ **M2-1..3 done (host + cross-link + alias-scan clean); live ping gate pending.** M2-1: `in_cksum` (RFC 1071) over a **PBUF chain** — word parity relative to `off` so a word straddling an ODD segment boundary is summed right (`src/nsfcksum.c`, alias `NSFCKSUM`; TSTCKSUM 10/10 on literal RFC/IP-header vectors, pinned before any packet code, spec 11.5). M2-2: **NSFIP** (`src/nsfip.c`) `nsfip_input` (validate v4/IHL/len/header-cksum; drop+count fragment→`fragdrop`, not-for-us→`inaddrerr`, bad cksum→`badcksum`, bad ver/IHL→`hdrerr`, bad len→`badlen`; demux ICMP, `noproto` TCP/UDP stubs; **TTL parsed but NOT a delivery gate** — RFC 1122 §3.2.1.7, `ttlexp` stays 0) + `nsfip_output` (build header in the PBUF headroom, monotonic id, computed cksum, route, `dev_send`) + fixed 16-entry **routing table** from HOME (classful on-link) + GATEWAY (default), longest-match, next-hop 0 = the point-to-point peer (`nsfip_config`/`nsfip_route_add`/`nsfip_local_add`/`nsfip_is_local`/`nsfip_route`; ADR-0024). **Every header field read/written BYTE BY BYTE** (big-endian), never a struct overlay/cast; addresses are `UINT`s (octet-1 in MSB) — the CTCI-codec discipline (green-and-wrong on the little-endian host otherwise). M2-3: **NSFICM** (`src/nsficmp.c`) echo responder in **the same PBUF** — verify ICMP cksum, flip type 8→0, recompute, strip IP header (opens exactly the headroom `nsfip_output` re-prepends), `nsfip_output` with src/dst swapped; `icmp_outecho` counted only on output success (ownership + counter never double up); non-echo/bad-cksum counted + freed. Seam wired: STC `EV_PACKET_RECEIVED`→`nsfip_input`; startup calls `nsficmp_init`+`nsfip_config` AFTER interfaces register (`src/nsfmain.c`). Host **569→641** (TSTCKSUM 10; TSTIP 39 — capture-DEVOPS + literal vectors + leak gate; TSTICMP 23 — NSFHOST-loopback echo round-trip with a verified reply + bad-cksum drop + leak gate); `-Wall -Wextra -Werror` clean; NSF module cross-links clean (cc370/as370/ld370), alias scan clean (11 new unique exports `NSFIP*`/`NSFICM*`/`NSFCKSUM`, statics ENTRY=NO). §11.1/11.4/11.7 + ADR-0024 + changelog v1.19 updated. **VALIDATED LIVE on MVSCE** (STC 165, real CTCI pair 0500/0501): `S NSF`→`NSF210I/211I` up; host `ping 192.168.200.1` **the from-scratch NSF stack answers** (`ttl=64`, ~1 ms RTT), counters exact — over 1000 packets `NSFIP in/out`, `NSFICM inecho/outecho` **all 1000**, every drop counter 0. Two things fell out of the live gate: (a) **a write-completion clear-race, FIXED** — the executive reap read+cleared the write subtask's IOB ECB `wecb` while the subtask polled it, stealing the completion and stalling the write pipeline after ONE frame (the read path never had this: the read subtask owns `recb`, hands up `rpost`/`rready`); fix makes write symmetric (`wpost`/`wready`, executive never touches `wecb`; CTCIDEV 108→112; TSTCTCI sustained-write regression, 44→168). (b) **The last frame of a burst stalls** — its reply is generated + queued (`outecho`/`NSFIP out` count it) but the write EXCP doesn't complete until the next inbound nudges the channel (`LNK1 out` lags by 1; ~half the RTTs ~200 ms). Hercules presents write CE+DE synchronously, so this is a **CTCI-driver write-subtask completion-wake** issue (M1 follow-up, [[nsf370-ctci-write-tail-stall]]), NOT M2 IP/ICMP. **So the M2 0-loss gate is 999/1000 — NOT claimed clean; the IP/ICMP layer is correct and the residual is tracked as an M1 driver follow-up.** Host **765/765**; TSTCKSUM/TSTIP/TSTICMP also **CC 0 on-MVS** (112/112, target byte-order proven). **M2-4 (ICMP errors) / M2-5 (stats+trace, fragment-drop) next.** |
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
| NSFDEV / NSFCTCI / NSFLCS / NSFHOST | Devices & drivers (NSFDEV table + DEVOPS + DEVIO seam + NSFHOST host driver, M1-2; CTCI top half `asm/nsfctcio.asm` (per-scb save areas) + SVC 99 seam `src/nsfctci.c` M1-3; codec `src/nsfctcif.c` + bottom half `src/nsfctcib.c` with the read/write **I/O subtasks** over the `nsfthr` seam (`src/nsfthr.c` / `src/nsfthr_host.c`) M1-4, ADR-0022/0023; host shims `src/nsfctcio_host.c`/`src/nsfctci_host.c`) | 9 | 200–299 |
| NSFSOC / NSFREQ | Sockets / Request mgr | 10 | 600–699 |
| NSFIP / NSFICM | IPv4 / ICMP — input validate/demux + output build/route + 16-entry routing table (`src/nsfip.c`, M2-2); ICMP echo responder in-place single-owner (`src/nsficmp.c`, M2-3); shared RFC 1071 checksum over a PBUF chain (`src/nsfcksum.c`, M2-1). ADR-0024; byte-wise big-endian, addresses UINT/octet-1-MSB | 11 | 300–399 |
| NSFUDP | UDP | 12 | 400–499 |
| NSFTCP | TCP | 13 | 500–599 |
| NSFCFG | Configuration | 14 | 700–799 |
| NSFEZA | EZASOKET API | 15 | 600–699 |
| NSFOPR | Operator interface (dispatcher + CIB seam; M0-8) | 5 / 17 | 800–899 |
| NSFMSG | WTO message seam (libc370 `wto`; M0-8) | 5 / 17 | — |
| NSFSTC | STC startup + NSFCFG→init wiring (M0-8) | 5 / 14 | 000–099 |
| (recovery) | ESTAE via libc370 `__estae` + C `nsf_recover` (ADR-0018; no NSFESTAE CSECT) | 17 | 900–999 |
