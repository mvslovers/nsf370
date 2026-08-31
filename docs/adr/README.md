# Architecture Decision Records

ADR-0001 … ADR-0013 are summarized normatively in
`../Architecture-Specification.md` §18; standalone files will be split
out as they are revisited. ADR-0014 onward are authored directly here.

**Two conventions for this file.**

1. **Status: merged means Accepted.** An ADR is `Proposed` until the PR carrying it is
   merged; the merge *is* the acceptance, so the status is flipped in the same change that
   records the countersign. An ADR whose decision was later replaced reads
   `Superseded by ADR-nnnn` — not `Accepted` — even though its PR was merged.
2. **THE ORDER BELOW IS CURATED. DO NOT SORT IT.** `0038` deliberately precedes `0037`,
   because 0037 is an intentional numbering gap and its row exists only to say so — sorting
   the table would bury that marker in the middle of the sequence where it reads as a
   missing file. Add new rows where they belong in the argument, not where a sort would
   put them.

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
| 0027 | Locally-originated writes actively park the armed READ via **IOHALT (SVC 33)** — problem-state, completion X'48' (purged) is the discriminator, not the residual; three read-completion classes + the ierr/nonip/rpurge counter split | ADR-0027-iohalt-active-read-park-for-local-writes.md |
| 0028 | UDP checksum via a **pseudo-header seed** (`in_cksum_partial`/`_fold`, not an overlay — the inbound PBUF has no headroom); IP demux by **registration** (`nsfip_register_proto`), which keeps NSFUDP out of the NSF module | ADR-0028-udp-checksum-seed-and-ip-demux-seam.md |
| 0029 | Socket API layering: thin facades over one **surface-neutral NSFEZA core**; the EZASOH03 veneer uses **PDPPRLG, not FUNHEAD** (the cc370 prologue reads the caller's DSANAB — a hand-rolled seam corrupts the save chain) | ADR-0029-ezasoket-facades-and-nsfeza-core.md |
| 0030 | Close issue #28: gate the IOHALT read-park on a **provably armed** READ (`rarmed`) — halting an un-armed read is a no-op in Hercules, so no X'48' arrives, `rhold` is never set and the WRITE stalls until the next inbound frame | ADR-0030-rarmed-guard-closes-issue-28.md |
| 0031 | M4-2 TCP connection machine: acceptq linkage via a **second TCB QELEM** (SOCKCB untouched), **background-close ownership inversion** (`tcp_destroy` never calls `soc_destroy`), and the synchronous-verb completion seams (`NSF_CLOSE_OWNED`) | ADR-0031-tcp-connection-machine-and-teardown-ownership.md |
| 0032 | M4-3 TCP data path: **copy-on-transmit** send / **trim-in-place** receive (single-owner PBUFs), signed sliding-window flow control with the window-update ACK that prevents deadlock, and FIN-after-data via `TCB_F_FINQ`/`FINSENT` | ADR-0032-tcp-data-path-buffer-ownership.md |
| 0033 | M4-4 TCP **retransmission** (fixed RTO + exponential backoff, one segment at SND.UNA — never re-blast the flight) and **zero-window persist** probes; rexmit ⊕ persist are never armed together, which is what makes a give-up teardown safe from a timer callback | ADR-0033-tcp-retransmission-and-persist-timers.md |
| 0034 | Timer arming/consumption contract: nsftmr_wake advances the armed ticks (fixes #40 tick-advance); empty⟺disarmed⟺g_armed==0; head-shortening re-arms without residual | ADR-0034-timer-arming-consumption-contract.md |
| 0035 | SELECT: one request over N sockets, readiness by a side-effect-free PROTOPS poll poked at the queue/state edge (M4-5) | ADR-0035-select-multiplex-and-readiness-poke.md |
| 0036 | Phase-2 app↔stack transport = the MVS SSI (dynamic SSCT/SSVT router, cross-AS __xmpost/WAIT, ESTAE-mandatory, runtime self-auth); five state/key rules inherited from UFSD verbatim — **transport superseded by 0038** (cross-AS rules retained) | ADR-0036-phase2-ssi-transport.md |
| 0038 | Phase-2 transport = a dynamically installed **private SVC** (stolen SVCTABLE slot, Type 3, no APF → serves **unauthorized** relink-only apps; SVC routine RENT HLASM in CSA, anchor published without SSI, slot restored at stop+abend); supersedes 0036's transport, reuses its cross-AS core | ADR-0038-phase2-svc-transport.md |
| 0037 | *(intentional gap — 0a′ was numbered 0038, skipping 0037; noted in ADR-0039)* | — |
| 0039 | Phase-2 `ubuf` cross-AS transfer = a keyed **CSA bounce** (no cross-memory; `MVCK` keyed copy if sound else key-0 memcpy — settled empirically in Stage-0b; chunk 2048 = `BUFLARGE`/PBUF-aligned, vs ufsd's 4K; staging embedded in the anchor, freed under drain/ESTAE) | ADR-0039-phase2-ubuf-csa-bounce.md |
| 0040 | Phase-2 client-death guard = an **ASVT liveness check immediately before the reply POST** (no RESMGR/RMTR on 3.8j): `asvtenty[asid-1]` available-bit **or** ASCB-address mismatch → DEAD → reap (never post into it); LIVE → post; **UNKNOWN → neither** (safe-side asymmetry — a live client called dead is the catastrophe). ASCB **address compare only**; ufsd #53's own-ASCB row deliberately **not** transferred (checker-is-server, not checker-is-client) | ADR-0040-phase2-client-death-guard.md |
| 0041 | Phase-2 **NSFRQE crossing** = copied into a CSA request slot (appended at +2184, nothing moves) and dispatched from an **STC-private key-8 copy** — so `soc_complete`'s SVC 2 POST never targets key-0 CSA and no socket/protocol file learns the boundary; `ubuf`/`ulen` **rewritten** in the private copy (staging address + the actually-staged count) which discharges ADR-0039's **moved-length obligation** through the already-frozen `retcode` — no new field, freeze holds; completion detected by an end-of-pass POSTED-bit check on the private ECB (parked ops), reply still gated by the 0040 ASVT guard. Single client / single slot by construction (pool = M5-2b) | ADR-0041-phase2-nsfrqe-crossing.md |
| 0042 | Phase-2 request slots = a **64-slot CSA pool claimed by per-slot compare-and-swap**, ABA-free because the location compared **is** the resource (hence no free list); three writers, three rules — claim by `CS`, release by plain `ST`, and **reap by `CS` because the reaper is a third observer** (two moves: `CS`→CLAIMED, clear, `ST` FREE). One request in service at a time (§10) | ADR-0042-phase2-slot-pool.md |
| 0043 | Phase-2 **cross-AS wake contract**: one STC-private key-8 ECB, POSTed by the client's SVC routine (`DOPOST`) and **consumed by `nsfsx_drain` at its head**, ahead of both scans (ADR-0022's reset half, which Phase 2 had never honoured); the executive is entitled to **block indefinitely** and NSF provides **no floor** — measured, not assumed (one pass in 259 s while still serving 8 cross-AS requests inside one console second). The reply POST is the mirror and is the one guarded by ADR-0040; conflating the two is the trap. Retires 64-2 as unnecessary. **Not** a fix for issue #64 | ADR-0043-phase2-cross-as-wake-contract.md |
| 0044 | NSFS makes itself **non-swappable**: `SYSEVENT DONTSWAP` at init, `OKSWAP` at shutdown (self-issued, no PPT entry required — measured: `OUCBASW` stays clear across an accepted DONTSWAP). Mitigates issue #64's twelve-minute swap-out outage and explains nothing; a refusal **warns and continues** (`NSF852W`), because this is a latency mitigation and not a correctness requirement | ADR-0044-nsfs-nonswappable.md |
| 0045 | The app-registry **reclamation sweep ships as best-effort, and is named that way**: client liveness is not soundly detectable on 3.8j (a batch client's identity is the **initiator's** and never dies; an STC's dies but is **resurrected** by ASID reuse), so the sweep reclaims only a slot whose ASID is absent from the ASVT at the moment of the check. **`TERMAPI` remains the contract**; "TERMAPI-equivalent" appears nowhere. Two triggers (periodic in `nsfsx_drain`, on demand when INITAPI finds no slot), one implementation, the cap as a parameter; the interval is **real seconds behind a seam** — not ticks (ADR-0034) and not a timer (ADR-0043). Interim measure; the design is deferred to issue #88 | ADR-0045-app-slot-best-effort-sweep.md |
