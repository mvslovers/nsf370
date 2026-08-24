# ADR-0039 — Phase-2 `ubuf` cross-AS transfer: a keyed CSA bounce

**Status:** Proposed (2026-07-22). Fixes how an application's data buffer (`ubuf`) moves
across the address-space boundary in Phase 2, on top of the private-SVC transport
(ADR-0038). Stage-0a′ proved the transport on an empty token; Stage-0b (this ADR) adds a
**data buffer** to the round trip and proves a byte pattern moves **app→stack→app
byte-exact** over the sizes a real `ubuf` will hit — so M5-2 marshals the frozen NSFRQE's
`ubuf`/`ulen` on a validated path. Still an isolated probe: **no NSFRQE, no socket, no
protocol code.**

**ADR number — the 0037 gap.** `ADR-0037` is unused on `main`: the Stage-0a′ ADR was
numbered `0038`, skipping `0037` (an off-by-one when it was authored). This ADR takes the
**next chronological number, `0039`**, rather than back-filling `0037`: ADR numbers read
chronologically, and a Stage-0b ADR numbered *before* the Stage-0a′ ADR (`0038`) it
depends on would misorder the dependency. **`0037` is left as an intentional, noted gap.**

**Relates to:** ADR-0038 (the private-SVC transport this rides — reused unchanged),
ADR-0036 (no cross-memory services on MVS 3.8j — the constraint that forces a CSA bounce
and rules out `MVCP`/`MVCS`), ADR-0009 (two buffer classes 256/2048 — the `BUFLARGE`
alignment for the chunk), spec §10.4 (the frozen NSFRQE `ubuf`/`ulen` this stages),
§17.3 (security — caller-pointer validation, M5-2). **Evidence pins:**
`ufsd/src/ufsd#fil.c` + `ufsd#que.c` + `ufsd#buf.c` (real file payload moved with **zero
cross-memory**: small data inline in the request block, larger via a CSA staging buffer
copied in the key-0 window, staging freed in the caller's key), `ufsd/include/ufsd.h`
(`UFSREQ_MAX_INLINE 256`, the 4K pool), the S/370 Principles of Operation for `MVCK` (Move
with Key, `D9`) semantics, and ADR-0038's countersign annotation (the open `MVCK` promise
this closes).

---

## Context

Phase 2 puts the application in one address space and the stack in the `NSFS` STC in
another. A real `send`/`recv` moves the application's `ubuf` bytes across that boundary.
MVS 3.8j has **no cross-memory services** (ADR-0036: no PC/PT, no SSAR, hence no
`MVCP`/`MVCS`), so the transfer cannot be a cross-memory move. UFSD already solved exactly
this for file payload — it does a **CSA bounce**: the SVC/SSI routine runs in the caller's
address space and can address the caller's key-8 buffer directly; it copies the data
into/out of a **CSA staging buffer** (common storage, visible to the STC) inside the
routine's key-0 window; the STC operates on the staging buffer; the boundary is crossed by
the **CSA rendezvous**, never by a buffer pointer.

Two questions this ADR settles for NSF:
1. **What instruction copies between the caller's key-8 buffer and the key-0 staging?**
   ADR-0038's annotation flagged `MVCP`/`MVCS` (DAS, absent here) as wrong and named
   **`MVCK`** (Move with Key — non-DAS, keys the *source* operand) as the candidate, with
   empirical confirmation assigned to Stage-0b. **Step 1 below settles it live.**
2. **How big is the staging buffer / copy chunk?** UFSD used 4 KB. NSF uses **2048**
   (`BUFLARGE`, the large PBUF, ADR-0009), so M5-2 copies straight into/out of PBUFs
   without a re-chunk.

## Decision

### 1. The `ubuf` transfer is a keyed CSA bounce (inherited from UFSD)

The request block `NSFV_REQ` (in the caller's address space, reached by `R1` at the SVC)
carries the **`ubuf` address + `ulen`, both in the caller's AS**, plus a new function code
`NSFV_REQ_XFER` (alongside `NSFV_REQ_ECHO`). The SVC routine, running in the caller's AS
in its key-0 window, copies the caller's `ubuf` **into** a CSA staging buffer, the STC
transforms the staging buffer, and the routine copies it **back out** to the caller's
`ubuf`. The 0a′ anchor / in-flight / timed-wait / slot-steal / drain / ESTAE machinery is
**reused unchanged**; only a staging buffer + a length + a function code are added to the
anchor, and the copy step is added to the routine.

### 2. The copy instruction: `MVCK` if Step 1 proves it sound, else key-0 `memcpy`

**Step 1 (do first) settles `MVCK` empirically** (see below). If `MVCK` executes and
honours the specified key, the app↔staging copies use **`MVCK`** (source keyed with the
caller's key 8 on the write-in read of `ubuf`) — it costs nothing extra and leaves M5-2
the right seam. If `MVCK` is absent or misbehaves, the copy falls back to a **plain key-0
`memcpy`**, and that is recorded here and in ADR-0038. **`MVCK` is the move instruction
(Stage-0b); caller address/length *validation*, `owner_ascb` checks, and fault recovery on
a hostile pointer are M5-2 (§17.3)** — Stage-0b's client is trusted and passes valid
pointers. Note that `MVCK` keys the *source* read, so the write-in (reading the caller's
`ubuf`) is genuinely key-checked; the write-out (storing into the caller's `ubuf`) runs
under the routine's PSW key 0 in Stage-0b — M5-2 tightens the write side (a PSW-key window
around the store) along with validation.

### 3. Chunk = 2048 (`BUFLARGE`), one staging buffer, multi-chunk = multi-round-trip

The CSA staging buffer is **2048 bytes** (`BUFLARGE`; UFSD's 4 KB is not `BUFLARGE`-
aligned, so NSF differs deliberately, so M5-2 copies into/out of the 2048-byte PBUFs with
no re-chunk). It is embedded in the anchor (CSA, SP=241, key 0), so it is freed with the
anchor under the existing **drain/ESTAE** discipline — no separate allocation/free. A
transfer larger than 2048 is **chunked**: the routine loops `[write-in chunk → round-trip
to STC → read-out chunk]` per 2048-byte chunk (each chunk reuses the 0a′ POST/WAIT). This
proves multi-chunk movement (the point of the PBUF-aligned chunk), not a single big copy.

### 4. A trivial, verifiable transform; truncation honoured

The STC applies a **byte-wise `+1`** to the staging bytes it was handed (`xlen` bytes) —
trivial but position-sensitive, so a short, wrong, or offset copy is *visible* in the
result rather than silently plausible. The routine honours `ulen` on the write-out: at
most `ulen` bytes are stored back into the caller's `ubuf`; a data run longer than `ulen`
is **truncated** (the first `ulen` bytes exact, the remainder discarded, **no overrun**).

### 5. `NSFV_REQ` / `NSFV_ANCHOR` extensions (layout, size-asserted)

- `NSFV_REQ` (20 B → 28 B): append `ubuf` (caller-AS address) + `ulen`; keep `eye`/`func`/
  `token`/`rc`/`seq`; the eye-catcher check is unchanged; the size assert is updated.
- `NSFV_ANCHOR` (120 B → 2176 B): append `xfunc` (which transform the STC applies), `xlen`
  (bytes in staging), and `stage[2048]` (the embedded CSA staging). The 0a′ 120-byte core
  (eye … `csasave`) is unchanged.

## Step 1 — settle `MVCK` empirically (closes ADR-0038's open promise)

A focused, controlled MVS probe answering:
1. **Does `MVCK` exist and execute** on MVS 3.8j under Hercules (no `S0C1` operation
   exception)? — a positive keyed copy, bytes exact.
2. **Does it honour the specified key** — does a key-mismatched access take a **protection
   exception** instead of a silent key-0 clobber? — the negative case, proven deliberately
   and controlled by an ESTAE that catches the exception (no dump): a page `SSK`'d to a
   key + fetch-protect that the `MVCK` source key (`R3`) does not match → `MVCK` faults.

The definitive result (executes? honours the key? which copy instruction Stage-0b
therefore uses) is **appended to ADR-0038 (append-only)** — that annotation currently
carries the open promise, and Stage-0b closes it.

## Why

- **UFSD-proven on this exact target**: the CSA-bounce mechanism moves real file payload
  today with zero cross-memory. NSF inherits it rather than inventing a scheme.
- **No cross-memory (ADR-0036)**: `MVCP`/`MVCS` are DAS instructions absent here; the
  bounce + `MVCK` (a base-S/370 keyed move) is the correct primitive set.
- **`BUFLARGE` chunk**: 2048 aligns the staging with the large PBUF so M5-2's marshalling
  copies straight into/out of PBUFs — the whole reason to fix the chunk now.
- **The seam for M5-2**: proving the byte-exact move + the `MVCK` keyed copy now means
  M5-2 adds only validation/security and the NSFRQE binding, not a new transfer mechanism.

## Consequences

- **The staging is CSA-shared, single-client-sequential** — like the 0a′ anchor scratch
  (`csasave`, ADR-0038): the bounce *requires* the staging in common storage (the STC must
  reach it), so it cannot live in the SVRB. Stage-0b stays single-client; **M5-2
  concurrency needs per-client staging** (multiple slots or per-request CSA allocation),
  the same caveat ADR-0038 already records for `csasave`. Stage-0b adds **no other** shared
  scratch.
- **Validation/security is M5-2, not here.** The routine dereferences the caller-supplied
  `ubuf` pointer; Stage-0b's client is trusted (valid pointers). Caller address/length
  validation, `owner_ascb` checks, RACF, and hostile-pointer fault recovery are M5-2
  (§17.3). `MVCK`'s source-key check is the seam; the write-out PSW-key window is M5-2.
- **`ubuf` chunking is NOT IP fragmentation** — it is a copy-granularity loop over CSA
  staging; it does not touch the no-frag path or the PBUF pools.
- **Anchor grows to 2176 B.** One CSA (SP=241) block; freed with the anchor under the
  drain/ESTAE discipline. `NSFV_SIZE_ASSERT` guards both structs at cross-compile.
- **Not host-simulable.** No SVC/ASCB/CSA/`MVCK` on the host; host coverage is the
  struct-layout asserts at cc370 cross-compile and the host suite staying **2788/0**.
  Proof is a live MVSCE run.

## Status / history

- **2026-07-22 — Proposed.** Authored ahead of the Stage-0b `ubuf` probe (extends `NSFV`).
  `0037` left as an intentional gap (see above). The `MVCK` result + the copy instruction
  actually used are recorded here and in ADR-0038 once Step 1's live run reports.
- **2026-07-22 — Step 1 done (`MVCK` confirmed sound; live on MVSCE, `TSTMVCK` 16/16
  CC 0).** `MVCK` exists, executes, copies byte-exact (keyed, `R3`=8, supervisor/key 0),
  enforces `R3` (a foreign key faults `S0C2`), and — on a genuinely fetch-protected frame
  (`LRA`→real, `SSK` key 0+FP, `ISK`-confirmed) — a key-8 read faults `S0C4` (the real
  hostile-pointer protection, demonstrated live at Mike's request). **Decision §2 resolves
  to `MVCK`** (not the `memcpy` fallback) for the `ubuf` app↔staging copies. Toolchain
  note: `as370` mis-assembles the `MVCK` mnemonic, so it is emitted as a **raw `D9`
  opcode** with the `R1`/`R3` register fields. Recorded append-only in ADR-0038 (closing
  its open promise). **Step 2 (the `ubuf` move) next.**
- **2026-07-22 — Step 2 done (the `ubuf` keyed CSA bounce; live on MVSCE, `TSTUBUF`
  21/21 CC 0 batch+TSO).** The SVC routine (`asm/nsfvsvc.asm`) gained the `XFER` path:
  dispatch on `req.func`, clamp `L = min(ulen, 2048)` (an IPL-class guard against running
  `MVCK` off the end of `stage[]` — the last anchor field — into adjacent CSA), stage
  `xlen = L`, **`MVCK` write-in** the caller's `ubuf` (source key 8, `R3`=`X'80'`) into the
  CSA staging (dst key 0) in ≤ 255-byte pieces, the reused `POST`/`WAIT`, then **`MVCK`
  read-out** the transformed staging (source key 0) back to the caller's `ubuf` (dst key 0,
  `L` reloaded from `ANCXLEN`). `MVCK` is a raw `D9` opcode (Step 1 toolchain finding);
  **255-byte pieces** deliberately avoid the ambiguous `R1`-low-byte-zero length (empirically
  the byte count is the `R1` low byte *directly*, so 255 is the safe max under both the
  0-means-0 and 0-means-256 readings); the piece length is saved in `R0` to advance so the
  loop never depends on a register `MVCK` might clobber. The `ECHO` path now also stages
  `xfunc = ECHO` so mixed `ECHO`/`XFER` traffic is not misdispatched by the STC. No new
  external symbols. **Live-proven** (MVSCE, unauthorized client `TSTUBUF`, `TESTAUTH`
  FCTN=1 == 0): a byte pattern moves app→CSA staging→STC(+1)→app **byte-exact** over sizes
  0 / 1 / 100 / 2048 (exactly one chunk) / 5000 (multi-chunk, 3 SVC round trips) / 10
  (short after the large transfer — truncation), the **guard byte after `ulen` untouched**
  in every case (no write-out overrun), all `XFER` chunk router `rc` OK; no abend (in
  particular none reading the `ubuf`), no `SYSUDUMP`. The STC drained to `INFLIGHT=0`,
  restored the stolen `SVC 239`, unloaded the CSA routine, and shut down clean; the
  `ECHO` regression (`TSTSVC`) stayed CC 0. **Deferred (M5-2, §17.3):** caller
  address/length **validation**, `owner_ascb` checks, and hostile-pointer **fault
  recovery** — Stage-0b's client is trusted; `MVCK`'s source-key check (proven in Step 1)
  is the seam. The **write-out** is key 0 in Stage-0b; M5-2 adds the PSW-key window on the
  store side. The staging stays **CSA-shared single-client** (like `csasave`); M5-2
  concurrency needs per-client staging. **Production adoption of the keyed move into the
  frozen NSFRQE `ubuf`/`ulen` marshalling is M5-2** — Stage-0b proves the mechanism, not
  the binding. **Stage-0b Step 2 complete pending Mike's countersign; the "Stage-0b
  proven" flip + the 0b PR merge are his. M5-2 stays unstarted until 0b AND 0c are green.**
- **2026-08-15 — Stage-0b COUNTERSIGNED and merged (PR #48, merged by the maintainer
  2026-07-22).** Per the repo convention (M3-5, M4-6) the maintainer's merge **is** the
  countersign, so the "pending countersign" qualifier above is discharged: **Stage-0b is
  proven.** The remaining gate before M5-2 is **Stage-0c — client-death cleanup**: what
  reclaims the slot, the in-flight count and the CSA staging when the client address space
  dies *between* the `POST` and the `WAIT` (`owner_ascb` is the seam; spec §17.3 and the
  M5-1 row of §19 "Roadmap" name the requirement, no stage has touched it). **M5-2 stays
  unstarted until Stage-0c is green.**
  *Reading note on the `TSTMVCK` count:* ADR-0038's status entry records **12/12** and the
  Step 1 entry above **16/16**. Both are correct at their moment — `test/mvs/tstmvck.c` has
  8 assertion sites and the runner counts batch + TSO, so 12 = 6 × 2 is the figure **before**
  scenario (3), the `S0C4` fetch-protect demonstration added at the maintainer's request,
  and 16 = 8 × 2 is the final one. **16/16 is the state of the test today.**
- **2026-08-15 — Two obligations M5-2 inherits from Stage-0b**, recorded here so they are
  not rediscovered by the next reader:
  1. **The write side of the keyed move.** Decision §2 and the Step 2 entry above already
     record the asymmetry, the PSW-key window as its fix, and the hostile-pointer fault
     recovery, all deferred to M5-2; two things they do not say. First, this is **M5-2's
     top security obligation**: `MVCK` keys only its *source* operand, so a wrong or
     hostile `ubuf` pointer takes a **key-0 clobber the hardware cannot catch** — the very
     failure the keyed move exists to prevent, left open on the store side. Second, the
     fix has **two admissible shapes**, not one: a PSW-key window around the store (§2's
     wording) *or* a destination-keyed move.
  2. **The moved-length contract is missing.** The routine clamps `L = min(ulen, 2048)`
     **silently**, and the chunk size is a constant shared between routine and client by
     convention, not by the interface — nothing in `NSFV_REQ` reports how many bytes were
     actually moved. BSD `send`/`recv`, which the frozen NSFRQE must present, return
     exactly that count. **M5-2 must return the moved length per call**; otherwise a
     client whose chunking assumption drifts from the routine's gets **silent truncation**
     where it should get a short count. (Stage-0b is safe only because its client and the
     routine share the constant and every size is asserted byte-exact.)
- **2026-08-23 — M5-2b Step 0: the destination-key probe (`TSTMVCD`; live on MVSCE,
  batch + TSO **CC 0**, **58 PASS / 0 FAIL** = 29 × 2).** This is the empirical probe
  obligation 1 above says the write-out key window "opens with". It **measures and
  decides nothing** — which mechanism M5-2b1 adopts is b1's call, made with this data.
  No production file was touched: `test/mvs/tstmvcd.c` + its `project.toml` entry only.
  Like `TSTMVCK` it is a **mechanism** probe and self-authorises (SVC 244), so the
  ADR-0038 unauthorised-client red line does not bear on it; unlike `TSTUBUF`/`TSTDEATH`
  it needs **no probe STC** (no anchor, no SVC, no rendezvous).

  **1. A destination-keyed move does NOT exist on this target.** The prediction recorded
  in the source before the run held: `MVCDK` and `MVCSK` both take an **operation
  exception — `S0C1`, `try` rc `000C1000`** — and neither touched its destination. Probed
  in **supervisor** state deliberately: from problem state a real `MVCDK` would first fail
  the CR3 key-mask check with `S0C2`, which is shape-identical to "absent", so supervisor
  state leaves the operation exception as the only expected failure. Corroborated from
  primary source *before* the run: Hercules `opcode.c` gates both `GENx___x390x900`
  (S/390 + z/Arch only — the S/370 slot of each is `operation_exception`) and MVSCE's
  `local.cnf` sets `ARCHMODE S/370`. The **whole SSE pair** is absent, not one opcode —
  the same shape of answer this ADR got for `MVCP`/`MVCS`.

  **Two corrections to the briefed hypothesis, both from primary source, both material.**
  The opcodes are **`E50F` (MVCDK) / `E50E` (MVCSK)**, not `B20F`/`B20E`. And the register
  convention is **R0 bits 24-31 = length MINUS ONE, R1 bits 24-27 = key** (Hercules
  `control.c`: `l = GR_L(0) & 0xFF` "operand length-1 from register 0"; `k = GR_L(1) &
  0xF0`), not "count in R1, key in R0". SSE is opcode(2) | `B1 D1` | `B2 D2`, so with
  B1 = 4 (destination) and B2 = 5 (source) the **listing bytes gated by `as370 -a=`** are
  `E50F40005000` and `E50E40005000`.

  **2. An `SPKA` window DOES close the write-out hole.** Supervisor state throughout,
  `SPKA` to the caller's key 8 around the access and back to 0; **`IPK` is read INSIDE the
  window on every arm and read `X'80'` every time**, so a non-faulting store could never be
  confused with "`SPKA` never took":
  - key-8 store into **key-8** storage **succeeds** and the bytes land — the baseline;
  - key-8 store into **key-0** storage **FAULTS `S0C4`** (protection, `try` rc `000C4000`),
    measured on **two independent destinations**: a `GETMAIN SP=241` block taken in key 0
    — the production anchor pattern, `ISK` key byte `X'06'`, i.e. key 0 — **and** one of
    the program's own frames `SSK`'d to key 0 and `ISK`-confirmed. Both faulted `S0C4`.
    **This is the check that matters, and it is unambiguous: the window protects.**
  - key-8 **fetch** from that key-0 CSA block **succeeds byte-exact** (`C3D4E5F6` read
    back), because the block is **not fetch-protected** — `ISK` byte `X'06'`, fetch-protect
    bit (`& X'08'`) clear. Behaviour and bit agree, asserted as such.

  So a window of this shape protects the destination *and* can still read the key-0
  staging source. The **destination** side was measured too: a job's own private storage —
  which is what `ubuf` and `rqeimg` are — reads `ISK` key byte `X'8E'`, i.e. **key 8,
  fetch-protected**, so under `SPKA` 8 it is the caller's own key on both counts and the
  fetch-protect bit is irrelevant to a store. **Inferred, not measured, and deliberately
  not a recommendation** (b1 owns the choice): with the key-0 CSA source readable under
  key 8 and the key-8 private destination writable under key 8, a plain `MVC` inside the
  window covers **both halves** of the move, which makes a third shape — `SPKA` to the
  caller's key plus `MVCK` with an explicit source key 0 — unnecessary in the common case.

  **3. CSA budget on MVSCE — with a correction: the GDA has NO CSA size field.**
  `SYS1.AMODGEN(IHAGDA)`, read on the live system, maps `GDA+0` flags, `+4 VRDREG`,
  **`+8 CSAPQEP`** — a *PQE pointer*. The size is one hop further, in the PQE (`IHAPQE`:
  `+8 PQEFPQE`, `+20 PQESIZE`, `+24 PQEREGN`). Chain used, and asserted in the probe:
  `CVT → CVTGDA (CVT+X'230') → GDA+8 CSAPQEP → PQE+20 PQESIZE / +24 PQEREGN`, with the
  `PQEFPQE` chain walked bounded because more than one PQE may describe the region.
  - **Total CSA = 2 113 536 bytes (2064 KB)**, described by **one** PQE, region at
    `X'009E0000'`.
  - **Largest `GETMAIN SP=241` that succeeds: ≥ 1 MB.** The coarse doubling search is
    capped at 1 MB by the probe itself and never failed, so this is a **floor**, not the
    maximum; powers of two understate the true largest by up to 2× in any case.
  - **The two numbers are different quantities and neither is *free* CSA.** b3 needs a
    **contiguous** allocation, which is the second number; against its ≈ 142 KB pool
    (64 × (2048 staging + 64 RQE + state)) the measurement is comfortable, but that
    reading is b3's to make. Every probe block was freed on every path; nothing is held.

  A **toolchain finding** came out of this and is recorded in CLAUDE.md §3: `as370` knows
  neither mnemonic and has **no SSE format at all**. No milestone flipped; §7 is unchanged.