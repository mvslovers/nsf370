# Architecture Decision Records

ADR-0001 … ADR-0013 are summarized normatively in
`../Architecture-Specification.md` §18; standalone files will be split
out as they are revisited. ADR-0014 onward are authored directly here.

| ADR | Decision | File |
|-----|----------|------|
| 0001 | Event-driven executive | spec §18 |
| 0002 | CTCI first | spec §18 |
| 0003 | Phase 1 in-process | spec §18 |
| 0004 | Fixed memory pools | spec §18 |
| 0005 | No Xinu code | spec §18 |
| 0006 | ESTAE + teardown checklists from M0 | spec §18 |
| 0007 | Prefix NSF, subsystem NSFS | spec §18 |
| 0008 | Single-owner buffers, no refcounting | spec §18 |
| 0009 | Two buffer classes (256/2048) | spec §18 |
| 0010 | Delta-queue timers, no wheel | spec §18 |
| 0011 | 100 ms tick via a single re-armed STIMER (not STIMERM) | ADR-0011-100ms-tick-via-stimer.md |
| 0012 | No IP fragmentation/reassembly in v1 | spec §18 |
| 0013 | Toolchain: cc370 + libc370, MBT V2 | spec §18 |
| 0014 | Build model & repo layout follow MBT V2 conventions | ADR-0014-build-model-and-repo-layout.md |
| 0015 | NSFMM pool regions via libc370 malloc (not raw GETMAIN) | ADR-0015-region-acquisition-via-libc370-malloc.md |
| 0016 | Shared nsftime seam (nsf_now + nsf_taskid) | ADR-0016-shared-platform-time-and-task-seam.md |
| 0017 | Timer wakeup via the async STIMER exit (not a subtask) | ADR-0017-timer-wakeup-async-stimer-exit.md |
| 0018 | Operator / WTO / ESTAE reuse libc370 seams (no hand-rolled asm) | ADR-0018-operator-wto-estae-via-libc370-seams.md |
| 0019 | CTCI completion via EXCP + IOB ECB, no CHE appendage | ADR-0019-ctci-completion-via-iob-ecb-no-appendage.md |
| 0020 | CTCI READ framing (one block, no 0x0000 terminator) + 3-hex-digit CUU | ADR-0020-ctci-read-framing-and-3-digit-cuu.md |
| 0021 | DEVIO ECB-completion seam (M1-4) — **superseded by 0022** | ADR-0021-devio-ecb-completion-seam.md |
| 0022 | Executive WAITs only on ECBs it owns (doneq via I/O subtask), not the raw IOB ECB — supersedes ADR-0019's WAIT premise | ADR-0022-executive-doneq-not-raw-iob-ecb.md |
| 0023 | CTCI I/O subtask implementation: raw-block doneq (Option B), single-block-sync READ, subtask-owned channel (per-scb save area), nsfthr seam | ADR-0023-ctci-subtask-implementation.md |
| 0024 | IPv4 host model: not-for-us→inaddrerr, TTL parsed-not-gated, routing from HOME/GATEWAY (peer=next-hop 0), addresses UINT + byte-wise wire | ADR-0024-ipv4-host-model-routing-and-address-convention.md |
| 0025 | Timed cross-task waits (timeout ECB in the waitlist) + CTCI pair sequencing (read re-arm behind the write pipeline) | ADR-0025-timed-waits-and-ctci-pair-sequencing.md |
| 0026 | nsffmt: safe vsnprintf/snprintf seam — libc370 does not NUL-terminate on truncation (issue #25) | ADR-0026-nsffmt-safe-formatting-seam.md |
| 0034 | Timer arming/consumption contract: nsftmr_wake advances the armed ticks (fixes #40 tick-advance); empty⟺disarmed⟺g_armed==0; head-shortening re-arms without residual | ADR-0034-timer-arming-consumption-contract.md |
| 0035 | SELECT: one request over N sockets, readiness by a side-effect-free PROTOPS poll poked at the queue/state edge (M4-5) | ADR-0035-select-multiplex-and-readiness-poke.md |
| 0036 | Phase-2 app↔stack transport = the MVS SSI (dynamic SSCT/SSVT router, cross-AS __xmpost/WAIT, ESTAE-mandatory, runtime self-auth); five state/key rules inherited from UFSD verbatim | ADR-0036-phase2-ssi-transport.md |
