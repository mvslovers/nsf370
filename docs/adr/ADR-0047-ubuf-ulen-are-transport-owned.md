# ADR-0047 — `ubuf` and `ulen` are transport-owned

**Status:** Proposed (M5-2, issue #101)
**Date:** 2026-09-02
**Supersedes / amends:** supersedes ADR-0035's `RQ_SELECT` **encoding** of
`ulen` (ADR-0035 as a whole stays Accepted); annotates ADR-0041 §2, whose
phrase *"the count actually staged"* is where the two meanings hid. The dual
of ADR-0003.

---

## 1. Context — the same field meant two things

`NSFRQE.ubuf` / `NSFRQE.ulen` are the frozen 64-byte request block's user-buffer
pair. In Phase 1 they are a pointer and a length in the caller's own address
space. In Phase 2 (ADR-0039 / ADR-0041) they describe a **keyed cross-memory
move**, and the transport that performs it — `src/nsfreqc.c`, `asm/nsfvsvc.asm`
`RQEIN`/`RQEOUT`, `src/nsfsx.c`, `src/nsfreqx.c` — **never reads `fn`**. It
cannot: the whole point of ADR-0003 is that the layers either side of it stay
ignorant of each other.

Every verb but one wrote `ulen` as a byte count. `RQ_SELECT` wrote an **item
count** (ADR-0035), because in Phase 1 that was free — `ubuf` pointed at the
caller's own array in the same address space and nothing measured it.

Phase 2 measures it. `RQEIN` stages `min(ulen, 2048)` **bytes**; `nsfsx.c`
copies `slot->xlen` **bytes** into the private landing area; `nsfreqx_dispatch_in`
sets `priv->ulen` to the count **actually staged in bytes**. A SELECT over N
sockets therefore crossed **N bytes** while `nsfsel_dispatch` read `r->ulen` as
an item count and walked **8N**. For N < 6 not even item 0's `ready` byte
(offset +5) reached the caller on the way back.

**The item count was right and the bytes were wrong**, which is why nothing was
internally inconsistent: `nitems <= NSFEZA_MAXSOC` (64) and the clamp is 2048,
so `staged == nitems` and the dispatcher received a plausible count over a
buffer holding one byte in eight.

**Observed** in M5-2d1's second live round (#100), as client B's own socket not
being reported ready.

### Why neither existing test could see it

`TSTREQX` pins the crossing and never builds a SELECT. `TSTSEL` pins the
dispatcher and never crosses a boundary. Each is correct in isolation. The
defect lives in the seam between them, and the gate for it had to be a binary
holding facade, crossing and dispatcher at once (`TSTEZA`, S12).

### What it cost while it stood

The poll form returned 0 ready. The timed form returned 0 ready one timeout
later. The **block-forever** form (`tv_sec < 0`, no `SEL_F_TIMED`) parked with
no timer, was re-scanned by `nsfsel_on_notify` forever without ever completing,
so `g_priv.ecb` was never posted and `g_busy` — set in `src/nsfsx.c`, cleared
only under `g_busy && g_busy_slot && POSTED` — was never cleared. **Every other
client's request then stayed PENDING for the life of the STC.** No abend, no
message, nothing to grep for.

It was **bounded, and the bound was an accident, not a check**: `sel_scan` read
and wrote up to `8n-1` in `g_land`, past the `xlen` bytes filled, and `g_land`
is `NSFREQX_CHUNK` (2048) while 8 × 64 = 512. Nothing outside a landing area
bounds it — see §6.

---

## 2. Decision

> **`ubuf` and `ulen` are transport-owned: `ubuf` is an address in the caller's
> address space and `ulen` is a length in bytes, for every verb. A
> verb-specific meaning belongs in `sockdesc`/`p1`/`p2`/`p3`, which the
> transport never interprets.**

Concretely:

- `nsf_select` (`src/nsfeza.c`) sets `r.ulen = nitems * sizeof(NSFSELITEM)`.
- `nsfsel_dispatch` (`src/nsfsel.c`) derives `n = r->ulen / sizeof(NSFSELITEM)`.
- A `ulen` that is **not a whole number of items is refused** `NSF_EINVAL`, not
  truncated (§4).
- `NSF_SIZE_ASSERT(NSFSELITEM, 8)` carries the size across the two separate
  compilations (§5).

`sizeof`, never a literal 8, on both sides. The NSFRQE layout is unchanged, the
anchor is unmoved, `ANCVERNO` stays 3, and no assembler was edited: `RQEIN` and
`RQEOUT` already move bytes and their `min(ulen, XFCHUNK)` arithmetic was
already correct.

---

## 3. Why this is ADR-0003's dual

ADR-0003 says the **protocol layer does not learn the transport** — `nsfudp.c`
and `nsftcp.c` must not know a boundary exists. ADR-0041 satisfies that
structurally, and ADR-0047's sibling defect (#80, the key-0 landing area) was a
failure on that side.

The other half has been an unstated habit: **the transport does not learn the
verbs.** Both halves of this family of defects sit on the unstated half, so it
is written down here. It is also why the fix is not a verb table: a table is a
thing that can be *wrong for a verb added later*, which is the same reason
ADR-0041 chose always-copy over a direction table.

---

## 4. The write-side and read-side audits

Verified by grep over `src/` at the time of writing, not inherited from a
summary. Exactly **five** verbs put anything in `ubuf`/`ulen`:

| verb | site | meaning |
|---|---|---|
| `nsf_sendto`   | `src/nsfeza.c:264-265` | bytes |
| `nsf_recvfrom` | `src/nsfeza.c:286-287` | bytes |
| `nsf_send`     | `src/nsfeza.c:416-417` | bytes |
| `nsf_recv`     | `src/nsfeza.c:434-435` | bytes |
| `nsf_select`   | `src/nsfeza.c:513-514` | **was items, now bytes** |

Fourteen verbs do not touch the pair at all. After this change all five mean
bytes.

**Six** sites read `r->ulen` in the protocol / dispatch layer:

| site | reads |
|---|---|
| `src/nsftcp.c:601`  | `tcp_send_resume` — bytes |
| `src/nsftcp.c:629`  | `tcp_recv_data` — bytes |
| `src/nsftcp.c:2014` | `tcp_send` — bytes |
| `src/nsfudp.c:199`  | `udp_complete_recv` — bytes |
| `src/nsfudp.c:268`  | `udp_sendto` — bytes |
| `src/nsfsel.c:197`  | `nsfsel_dispatch` — **was 8 × `ulen`, now `<= ulen`** |

(The kickoff's list of read sites omitted `nsftcp.c:2014`. It is a byte count
and correct, so no conclusion changed — but the audit is only worth having if
it is complete, so it is recorded here as six.)

The transport itself relays the pair without interpreting it: `src/nsfreqc.c:71`
copies it verbatim; `src/nsfreqx.c:74-75` rewrites it to the landing address and
the staged byte count.

**The audit is swept beyond `src/`, and it has to be.** An MVS-only test
(`host = false`) is **compiled but never run** by any host gate, and
`r.ulen = 2u` compiles perfectly — so a stale count-valued `ulen` in one would
survive every offline check and surface later as a live arm failing for a reason
unrelated to what that arm measures.

Over `test/` and `samples/`: every other `ulen` is already a byte length (TCP and
UDP buffers, XFER staging, the crossing vectors). Four test files reference
`RQ_SELECT`: `test/tstsel.c` and `test/mvs/tstd1b.c` hand-build the request and
are **edited here**; `test/host/tsteza.c` is the gate; `test/tstreq.c` never sets
`ulen` at all (it only proves `RQ_SELECT` completes `EOPNOTSUPP` with no engine
registered). `test/mvs/tstezat.c` — the M4-5 live SELECT gate — goes through
`nsf_select`, and so does the EZASOH03 `SELE` decoder (`src/nsfeza.c:717` is
`return nsf_select(...)`, not its own RQE). Which is why the SELECT write of
`ulen` appears **exactly once** in the whole tree, and why the facade is the only
place that needed changing.

### The `src/nsfsx.c` residue bound (the M5-2b1 / #80 clause)

`nsfsx.c`'s copy-out comment argues that the range copied back is exactly the
range the copy-in filled with **this** client's own staged content, so a
previous client's bytes in `g_land` are never handed across address spaces.

That argument is true of the copies, and its scope was implicit: it holds
**because every read length in the table above is bounded by the staged byte
count**. `nsfsel.c:197` was the one read that was not, and it is now. The
sentence at that comment is **not refuted** and is not annotated as superseded;
a pointer to this ADR is added so the dependency is visible.

---

## 5. `NSF_SIZE_ASSERT(NSFSELITEM, 8)` is load-bearing, not decorative

The facade links into the **application's** load module; the SELECT engine links
into the **STC's**. They are separately compiled and separately linked, and
CLAUDE.md §5 documents how easily one of the two gets redeployed without the
other.

Within one compilation the facade's multiply and the dispatcher's divide cancel
**whatever `sizeof(NSFSELITEM)` happens to be** — so a host test, which links
both halves into one binary, is structurally incapable of detecting a
disagreement. The assert is the only thing that carries it.

It is also **an existing invariant that had not been followed**, not an
improvement: CLAUDE.md §3 *Memory* already requires that every control block
declare its byte size and be guarded by `NSF_SIZE_ASSERT`.

**Where it fires, stated precisely.** `NSF_SIZE_ASSERT` is a no-op off-target by
design (`include/nsf.h`: a host build has 8-byte pointers, so asserting target
sizes there would fail to compile every control block containing one). It
enforces under `__MVS__` — i.e. on the **cross build**, which is exactly the
build that produces the two load modules whose agreement is at stake. Verified
to fire rather than assumed: changing the 8 to a 9 leaves the host suite
**green at 3491 PASS** and fails the cross build with
`include/nsfsel.h: error: size of array 'nsf_assert_NSFSELITEM' is negative`.
That the host stays green while the target build breaks is the point of §5 in
miniature.

---

## 6. Why a non-multiple is refused, not truncated

The transport moves `min(ulen, 2048)` bytes and never inspects `fn`, so a
partial trailing item is a request the engine cannot honour. Truncating would
report a **smaller set as if it had been asked for** — the silent-narrowing
shape this project has paid for repeatedly.

`NSF_EINVAL` is not a choice so much as the existing convention: `include/nsfreq.h`
already states that the dispatcher completes an unknown or invalid request
`NSF_EINVAL`, and `soc_dispatch` does exactly that for an unknown `fn`.

**The rejection is reachable only from a client that hand-builds its RQE.** The
facade always multiplies, and it caps at `NSFEZA_MAXSOC` (64 items = 512 bytes)
against a 2048 clamp that is itself a multiple of 8 — so neither a non-multiple
nor a truncation can be produced through NSFEZA. The shapes that can reach it
are `test/mvs/tstd1b.c`'s (a test that encodes its own request) and a hostile
one. That is also why both controls in the S12 gate are hand-built rather than
driven through `nsf_select`.

---

## 7. Consequences

- **No new field, no layout change.** `NSF_SIZE_ASSERT(NSFRQE, 64)` holds,
  `rsvd`@60 is unspent, the anchor is unmoved, `ANCVERNO` stays 3. Apps relink
  only.
- **Phase 1 and Phase 2 go through the same code.** There is no per-phase
  branch: in Phase 1 the multiply and the divide simply cancel in the same
  address space.
- **Phase 1 behaviour is unchanged**, but the `NSF` and `NSFECHO`/`NSFTECHO`
  modules link the changed `src/nsfeza.c` and `src/nsfsel.c`, so their bytes
  differ. "Unchanged" here is behavioural, not byte-level (the M5-2c1 wording).
- **`test/mvs/tstd1b.c` moves with the facade** — its two hand-built `ulen`
  values (`2u`, `1u`) are not multiples of 8 and would now be refused. Edited in
  the same change; the file is `host = false`, so that edit is **unverified in
  Stage 1** and gets its proof in the live round.
- **The named next candidate is `selectex()`** — an ECB list is an array of
  pointers crossing through `ubuf`, the identical fork, and it would arrive with
  the same temptation to count elements. Noted at `docs/nsf370-provider-contract.md`
  §4 and its open question 6. (The kickoff also named a `docs/socket-provider.md`;
  no such file exists in the tree, so the note lives only in the one that does.)

---

## 8. What this ADR does not establish

- That a cross-AS SELECT works. The host gate proves the length arithmetic, the
  array integrity, the refusal of a non-multiple and the truncation behaviour —
  all in one address space. Whether the crossing carries a SELECT correctly on
  the machine is the live round's question (M5-2d1 §2.3), and #87's territory.
- That the two load modules agree on `sizeof(NSFSELITEM)`. That is §5's assert,
  and it fires at compile time on each side separately; no host run tests it.
- Anything about the `g_busy` wedge beyond its cause. **It is read from source
  and has never been run** — observed neither happening nor gone. §1's
  description of it is a deduction whose every hop was checked (no timer on the
  block-forever form, `sel_scan` resolving nothing, `sel_finish` never running,
  `g_priv.ecb` never posted, `g_busy` cleared only under `POSTED`), and a
  deduction is not a measurement however many hops hold. It is re-cast as the
  **written prediction for Stage 2 arm 3**, recorded in
  `docs/measurements/m5-2-101/` before that run. The wedge is a consequence and
  goes away with the cause; the serial-service property it exposes is
  ADR-0042 §10's, and measuring it is (e)'s job.

  **The rule, recorded rather than promoted** (its home in CLAUDE.md §8.5 or
  elsewhere is the maintainer's call): *a chain read out of source is a
  prediction until a run, however many of its hops were verified.* It is a
  different failure from an unchecked property or an assumed mechanism — there
  is no wrong hop here — and **the tell is the tense of the sentence, not the
  quality of the reasoning**.
