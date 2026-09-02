# M5-2d1 — second live round: instruments repaired, then STOPPED at §2.3

**Status: instruments repaired and each shown to work. §2.4 cases 1 and 3 GREEN;
case 2 NOT ESTABLISHED. §2.2b GREEN against A's real descriptor. §2.3 STOPPED —
it exposed a product defect, and the round's red line is report and stop.
§2.5 and §2.6 NOT RUN, because the stop came first.**

Branch `m5-2d1-live-b`, on `main` after #99 (`da04669` + countersign `6046003`).
**No product source was changed** (`git diff src/ include/ asm/` empty).

The stand sequence was written down before touching the stand
(`sequence-b.md`) — the first round's `ABEND SFEF` came from stopping NSFS
inside A's hold window, and **that did not recur**.

---

## 1. The instrument repairs, each shown to work

| defect | repair | evidence it works |
|---|---|---|
| 1 — sweep counted B's own socket | **sweep BEFORE owning anything**, so any hit is foreign *by construction*; and record *which* descriptors, not how many | `SWEEP pre-own 0 REACHED` |
| 2 — a count from a range that misses the target reads the same as a refusal | after creating its own socket, sweep again; the range is adequate **only if** the new socket appears, else say so and skip rather than report a zero | `SWEEP post-own 1 REACHED (first 00010001)` — the positive control fired |
| 3 — §2.2b/§2.3 aimed at a constant | derive A's descriptor **at run time** from B's own (same generation, index − 1) | `OWN 00010001 A-DERIVED 00010000` — and `00010000` is exactly what the **previous round's revert arm** named as A's, so the derivation is corroborated by an independent measurement |

**§2.2 now means something it did not before:** owning nothing, B reached **0 of
128** while A demonstrably held a socket, *and* the range was proven able to
reach a live descriptor moments later. Its **discrimination** still rests on the
previous round's revert arm (2 hits, naming `00010000`); this round did not
re-run it, because the stop came first.

## 2. §2.4 — the R8 validation, live for the first time

Ran against **NSFV, not NSFS, and that was forced**: case 2 needs the `XFER`
verb, and M5-2c0 established a non-RQE probe verb at NSFS goes `HELD` and the
client **parks forever** — issue #67's shape. **This test walked straight into
it on its first run and had to be cancelled** (`C MBTTEST`), which left a slot
in flight and made the next `P NSFS` take the retain branch
(`NSF054W`, CSA + router retained to IPL). That is #67 costing real time, in the
round that was told not to fix it.

| case | result |
|---|---|
| **(1) no eyecatcher** | **PASS** — refused, and the block **not written**: both sentinels survived (`rc=5AC0F001 seq=5AC0F001`). "Refused" and "refused after writing" are different answers, and this distinguishes them. |
| **(2) eyecatcher at an address the caller does not own** | **NOT ESTABLISHED** — see below |
| **(3) the never-referenced tail** | **PASS** — block at `00111FF8`, page boundary at `00112000`, so offsets 0-7 are in one page and 8-63 in the next, and the client wrote **only** 0-7. Accepted, `rc=0`. This is the case the CC-3 correction exists for, and it is the shape that never appears in a suite and appears occasionally in the field. |

### 2.1 Case 2 is NOT ESTABLISHED, and the self-validation is what says so

The plan was to stage a block beginning `"NSFV"` into the client's own CSA
staging buffer, then point R8 at it — key-0 storage carrying a valid eyecatcher,
so only the key check could refuse.

**The self-validation failed and the case must not be quoted.** Measured:

```
case 2: staged into slot 0, target = 00AAD860
case 2: target[0..7] = D6 E3 C7 E6 01 01 01 05
FAIL: 2.4(2): the target is key-8 READABLE and carries "NSFV"
```

`D6 E3 C7 E6` is EBCDIC **"NSFV" with every byte + 1** — the `XFER` verb applies
its `+1` transform to the staged data, so the staging path **cannot place a
valid eyecatcher**. The refusal that followed is therefore explained by the
*eyecatcher* check, not the *key* check, and the `PASS` printed under it is
**not evidence for TPROT**.

Two things the case did establish, and they are worth keeping: the target **is**
key-0 (a key-8 store into it faults — proved with `___try`, not assumed), and
the refusal left the block untouched.

**The fix for the next round is concrete:** stage the eyecatcher **minus one**
(`D4 E1 C5 E4`) so the `+1` yields `D5 E2 C6 E5`. **One loose end must be
resolved first, not explained away:** the diagnostic's "wanted" bytes printed as
`4E 53 46 56`, which is **ASCII** `NSFV`, while the staged-and-transformed value
implies the source was **EBCDIC** `D5 E2 C6 E5`. Both cannot be true of the same
literal. That contradiction is unresolved and the next round must settle it
before trusting anything built on `NSFV_REQ_EYE`.

## 3. §2.3 — STOPPED, because it exposed a product defect

**`ulen` means two different things and nothing reconciles them.**

- `src/nsfsel.c:197` — `UINT n = (items != NULL) ? r->ulen : 0u;` — NSFSEL reads
  `ulen` as an **item count**.
- `src/nsfeza.c:513-514` — `r.ubuf = items; r.ulen = nitems;` — the EZASOKET
  facade sets it to the **item count** too.
- The cross-AS transport stages `min(ulen, 2048)` **BYTES**
  (`RQEIN` → `SLXLEN` → `nsfreqx_stage_len`), and `nsfreqx_dispatch_in` then
  sets `priv->ulen` to the **count actually staged**.

So a cross-AS SELECT over *N* sockets stages **N bytes** instead of
*N* × `sizeof(NSFSELITEM)` = *N* × 8, and NSFSEL then reads *N* items out of an
*N*-byte buffer. Measured: a 2-item mask came back
`foreign.ready=0 own.ready=0` — **B's own socket was not reported ready either**,
which is what exposed it.

**This is not a test artifact.** The facade sets `ulen` the same way, so a real
EZASOKET client doing a cross-AS SELECT hits it identically.

**It is the same family as #80 and was named as a gap by 80-FIX**, which
observed that no test drives a cross-AS SELECT and that the always-copy design
covered its *key* problem "without anyone having had to know it existed". The
key problem was covered. **The length problem was not.**

**Consequence for this round: §2.3 cannot be measured until this is resolved.**
The results printed above are about a truncated item array, not about ownership,
and are not quoted as evidence. Per the red line, **no product change was made**.

## 4. NOT RUN

- **§2.5** — the three-state revert covering both checks. The stop came first,
  and reverting to measure a case that cannot be measured would prove nothing.
- **§2.6** — Phase 1 live.
- **§2.2's own revert arm** this round; its discrimination rests on the previous
  round's.

## 5. Stand

NSFS up (STC 1723, `SVC 239 STOLEN`, `NSF001I`), `F NSFS,STATS` →
`BUSY=0 INFLIGHT=0 EXHAUSTED=0 COLLISIONS=0 REAPED=0`, **zero dumps**, no jobs
in flight. **One debt:** the cancelled TSTD1R run left a slot in flight, so
`P NSFS` took the retain branch and **one anchor plus the router are retained
until IPL** (~139 KB of CSA). Caused by #67, in the round that was told not to
fix it.
