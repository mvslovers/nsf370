# #101 Stage 1 — `ulen` becomes a byte length for every verb (host only)

**Date:** 2026-09-02 · **Base:** `main` @ `3884440` · **ADR:** 0047
**Proof kind: HOST ONLY.** No deploy, no MVS run, no live claim. The live proof
is Stage 2 (M5-2d1 §2.3) on its own branch and its own PR.

---

## Files here

| file | what it is |
|---|---|
| `gate-red-unmodified.txt` | TSTEZA on **unmodified product code**, gate added: 172/182, **10 named FAIL** |
| `gate-red-product-reverted.txt` | TSTEZA with the two product lines **reverted** and everything else in place |
| `host-suite-product-reverted.txt` | the whole suite in that state, per-test + total, **one invocation** |
| `gate-green-fixed.txt` | TSTEZA with the fix: **182/182** |

---

## The three states, one axis

The axis is exactly two product lines: `src/nsfeza.c`'s `r.ulen = …` and
`src/nsfsel.c`'s derivation of `n` (with its rejection). The gate, the size
assert and the updated tests stay in place across all three.

Every figure below is from a **clean rebuild** (`make clean` first), and each
row's total and per-test numbers are from the **same invocation**.

| state | host suite | TSTEZA | TSTSEL |
|---|---|---|---|
| baseline, no gate | 3469 PASS / 0 FAIL | 160/160 | 73/73 |
| **gate added, product UNMODIFIED** | 3481 PASS / **10 FAIL** | 172/182 | 73/73 |
| **fixed** | **3491 PASS / 0 FAIL** | 182/182 | 73/73 |
| **product reverted** | **3408 PASS / 10 FAIL** | 172/182 | **aborts, 0/0** |
| restored | 3491 PASS / 0 FAIL | 182/182 | 73/73 |

The reverted row reconciles: the per-test PASS column sums to **exactly 3408**,
and the 10 FAIL are exactly TSTEZA's.

**The reverted state fails in two different ways, and the second is worth
naming.** TSTEZA gives the same 10 named failures. **TSTSEL aborts** — rc 134
(SIGABRT), 0 assertions reported, its buffered stdout lost with the abort —
because its arrays are exact-sized locals: `test/tstsel.c` passes a byte length
of 24 over an array of 3 items, and a dispatcher reading that as an item count
walks `24 × 8 = 192` bytes over a 24-byte stack array. So the mismatch is a
**memory-safety fault**, not only a wrong answer. Repeated three times on that
binary: rc 134 every time, 0 assertions every time.

**The revert is deliberately ASYMMETRIC, and that is the correct revert.** The
two product lines go back; the gate, the size assert and the updated test files
stay new. That is what isolates the axis — and it is precisely why `TSTSEL`
walks 8× its arrays in that state, since `test/tstsel.c` now passes byte lengths
to a dispatcher reading counts. It is not an accident of the procedure.

### The abort hides inside the measuring instrument (§8.5, one level up)

**Both red states report `10 FAIL`.** Read the FAIL count alone and the reverted
state is indistinguishable from the first; the only tell is the PASS total
dropping by TSTSEL's 73 assertions.

Read from the harness (`mbt/scripts/mbttesthost.py`), not inferred:

- `ok = (run.returncode == 0)` — so an aborted test is **loud twice**: its
  per-test row reads **`FAIL rc=-6`**, distinct from an assertion failure's
  `rc=1`, and `make test-host` exits non-zero.
- the assertion totals are `len(_PASS.findall(spool))` / `len(_FAIL.findall(spool))`
  over the test's **captured stdout** — which the abort discarded. So the test is
  reported and its **73 assertions vanish from the totals**.

So it is **loud at the per-test row and in the exit code, silent in the assertion
totals**. The row sits directly above the totals line, so a reader who reads both
is not misled; a reader who compares only `10 FAIL` to `10 FAIL` would conclude
the revert reproduced the original state exactly, and it did not.

**Proposed, not implemented** (and it is an `mbt` change, a different repo, so
Mike's call whether it belongs in the harness and in §8.5): a row with
`ok == False` **and** zero assertions is exactly the absent-vs-succeeded shape,
and the harness already holds both facts at the point it formats the row —
labelling that case distinctly (rather than as an ordinary `FAIL`) is the
smallest change that would make it loud.

**A superseded figure, recorded rather than quietly replaced.** An earlier
*incremental* (no `make clean`) run of this same state reported **3479 PASS / 12
FAIL**, and its per-test listing came from a second invocation — so the two
halves did not reconcile. Both were rerun from clean into the single invocation
above. The earlier pair is superseded; no mechanism is claimed for the
difference, because none was established.

In the real Phase-2 crossing the array lives in `g_land` (2048 bytes) and 8 × 64
= 512, so the overrun there is **bounded — by accident, not by a check**. That
is the difference between the two, and it is why the production symptom was a
silent wrong answer rather than a crash.

---

## The red arm, verbatim

```
FAIL: cross: 2 items staged as 2*sizeof(NSFSELITEM) bytes (got 2, want 16)
FAIL: cross: dispatcher handed a BYTE length covering both items (got 2, want 16)
FAIL: cross: item 0 desc arrives as the facade built it (got 2779054080, want 720896)
FAIL: cross: item 0 want arrives as the facade built it (got 165, want 1)
FAIL: cross: item 1 desc arrives as the facade built it (got 2779096485, want 327681)
FAIL: cross: item 1 want arrives as the facade built it (got 165, want 1)
FAIL: cross: both sockets reported ready (got 0, want 2)
FAIL: cross: read mask returns both bits set
FAIL: cross/odd: a non-multiple ulen is refused
FAIL: cross/odd: refused NSF_EINVAL (the dispatcher convention) (got 0, want 22)
```

**Read the numbers, they name the mechanism.** `0xA5` is the landing/staging
residue pattern the gate pre-fills.

- `got 2, want 16` — two items crossed as **2 bytes**, the count taken for a
  length.
- item 0's descriptor arrives as **`2779054080` = `0xA5A50000`**: two real bytes
  and two residue bytes. **Item 0 is corrupted, not merely the tail** — which
  refutes the natural reading that truncation only loses later items.
- item 1's descriptor is **`2779096485` = `0xA5A5A5A5`**: nothing of item 1
  crossed at all.
- item 0's `want` is **`165` = `0xA5`** — residue, not the `SEL_READ` the facade
  wrote.
- `rc 0` instead of `2`: `sel_scan` resolved residue as descriptors,
  `nsfreq_sock_owned` returned NULL for both, so **no socket was reported
  ready**. That is #100's live symptom, reproduced host-side.

---

## Why the gate had to be new

`TSTREQX` pins the crossing and never builds a SELECT. `TSTSEL` pins the
dispatcher and never crosses a boundary. Both were green throughout, before and
after. The defect is in the seam, so the gate is the first binary holding
facade, crossing and dispatcher at once — `TSTEZA` S12, which is why
`src/nsfreqx.c` joins that one source list.

**The oracle is the facade's own output**, not a predicted descriptor: the
modelled transport snapshots the item array on the way in and the
dispatcher-side view on the way out, and asserts the array arrives as built. It
asserts on the **dispatcher-side** view because the copy-back overwrites the
caller's array, which would muddy which hop lost the data.

### The facade's multiply IS inside the tested path

The gate pins the **multiply and the divide**, not the divide alone — so
"the joined gate is green" is not carrying a claim the construction does not
support.

The request under test comes out of **`nsf_select`**
(`test/host/tsteza.c:753`), with the transport registered around exactly that
call. The recursion (below) was fixed **strictly below the facade**: `x_exec`
stands the transport down only for the inner dispatch of the *STC-private copy*,
which is the executive's own dispatch, not the facade's request. Nothing was
hand-assembled and nothing was called below the facade for the gate itself.

**And that is checkable, not merely structural.** The gate's first assertion
reads `nsfreqx_stage_len(r->ulen)` off the **facade's own RQE**
(`tsteza.c:691`), and on unmodified code it reported **`got 2`** — the
un-multiplied item count. Had the facade's encoding been outside the gate, that
assertion could not have moved.

The two **controls** are hand-built, and have to be: the facade always
multiplies and cannot produce either shape (see below).

**And the structural half, which the behavioural one does not cover.**
`nsfreq_call` consults `g_xtransport` **exactly once, before dispatch, and never
again on the return leg** — `src/nsfreq.c:979-982` is
`if (g_xtransport != NULL) { g_xtransport(r); return; }`. So standing the
transport down inside `x_exec` cannot change what the facade does with the
result: the model is faithful on the way **back** as well as on the way in. The
behavioural argument above is the discriminating one; this is the assurance that
there is no second consultation to have missed.

### Two things about the gate's construction

- It uses the **poll form with both sockets already ready**, so
  `nsfsel_dispatch` completes inline and never arms a timer. TSTEZA runs a
  drainer thread; a parked SELECT would race it. `test_select_masks` solved this
  the same way.
- Its landing buffer is `NSFREQX_CHUNK`, like the real `g_land` — **not** the
  staged length. On unmodified code the dispatcher writes `ready` past the
  staged bytes, and that must surface as the named assertion above rather than
  as an overrun of the test's own buffer.

### `0xA5A50000` is a host artifact — the assertions are endian-free

`TSTEZA` is `mvs = false`. With descriptor 0 the corrupted item 0 is the bytes
`00 00 A5 A5`, which a **little-endian host** reads as `0xA5A50000`; the same
corruption on the target reads `0x0000A5A5`. **The defect is endian-free, the
number is not** — so it is kept here as the observed diagnostic, and a reader
should not chase that constant on MVS.

**No assertion compares against a corrupted constant.** Every item check is
`g_x.seen[k].desc == g_x.sent[k].desc` and `.want` likewise — *survival* against
what the facade wrote. The corrupted value appears only in `CHECK_EQ`'s
auto-generated failure message. The remaining literals are all the *correct*
expected values (`2 * sizeof(NSFSELITEM)`, `NSFREQX_CHUNK`, the `0x03` mask,
`NSF_EINVAL`), and the `0xA5` fill is a byte `memset` — endian-free.

**The fill pattern is load-bearing.** A **zeroed** landing buffer would make
residue read as descriptor **0** — a *valid* descriptor — and the gate would have
been asserting against a plausible answer instead of an obvious one.

### A trap found while building it

`nsfreq_submit` / `nsfreq_wait` **short-circuit back into the registered
transport** (`src/nsfreq.c`: on a cross-AS transport the whole round trip happens
inside the transport, so the paired wait has nothing to do). Calling them from
inside the modelled transport re-entered it — unbounded recursion that **blew the
stack instead of failing an assertion**. The fix (`x_exec`) stands the transport
down for the inner dispatch, which is also the truthful model: inside the
transport we are the STC, and the STC reaches its executive through the ordinary
queue.

---

## Controls, each distinguishable from a test that never ran

| control | result |
|---|---|
| empty set (`ubuf == NULL`) | stages nothing, dispatcher sees a zero length, `rc 0`, no fault |
| non-multiple `ulen` (9) | refused, `retcode < 0`, `errno == NSF_EINVAL` (22) — **not** the empty set's `rc 0` |
| truncation (`ulen` = 2112 > 2048) | `priv.ulen` clamped to 2048, a whole multiple of 8, derived count 256 = the bytes that crossed |

**The truncation control pins arithmetic consistency, not dispatcher behaviour
on a truncated set — and does not need to.** `NSFREQX_CHUNK` is **2048, itself a
multiple of 8**, so the clamp can never manufacture a non-multiple out of a
well-formed request. Truncation and refusal are disjoint, and the refusal path is
reachable only from a hand-built RQE.

**Both the non-multiple and the truncation controls are hand-built requests, and
they have to be.** The facade always multiplies, and it caps at `NSFEZA_MAXSOC`
(64 items = 512 bytes) against a 2048 clamp that is itself a multiple of 8 — so
neither shape is producible through `nsf_select`. They are the shape a client
that encodes its own RQE has (`test/mvs/tstd1b.c`) and the shape a hostile one
has.

---

## Offline gates

- host suite **3491 PASS / 0 FAIL**, 27 tests (baseline 3469; **+22, all in
  TSTEZA**, 160 → 182)
- `-Wall -Wextra -Werror` clean, clean rebuild
- **ASan + UBSan clean** on TSTEZA, TSTSEL, TSTREQX (the three affected).
  `TSTTRC` reports 0 assertions under sanitizers — **pre-existing and unrelated**:
  it links `tsttrc.c` / `nsftrc.c` / `nsffmt.c` / `nsftime.asm`, none of which
  this change touches.
- cross-build clean: **6 modules + 56 test modules** (cc370 / as370 / ld370)
- alias scan: **246 unique, all ≤ 8 chars, none added**
- `tools/check-card-columns.sh` OK; `tools/check-sock-lookup.sh` OK (2 call
  sites, all classified)
- `NSF_SIZE_ASSERT(NSFSELITEM, 8)` **verified to fire**: changing the 8 to a 9
  leaves the host suite green at 3491 and fails the **cross** build with
  `include/nsfsel.h: error: size of array 'nsf_assert_NSFSELITEM' is negative`.
  The macro is a no-op off-target by design (`include/nsf.h`), and the cross
  build is the one that produces the two load modules at stake.

---

## What is host-verified, and what is not

**Host-verified:** the length arithmetic on both sides; that a two-item array
survives the crossing intact; that a non-multiple `ulen` is refused `NSF_EINVAL`
and is distinguishable from an empty set; that truncation keeps the derived
count consistent with the bytes that crossed; that the end-to-end mask round
trip reports both sockets ready.

**NOT established by any of it:**

- **That a cross-AS SELECT works at all.** Everything above happens in one
  address space against a *modelled* transport. The machine's answer is Stage 2
  (M5-2d1 §2.3), and #87's territory.
- **That the refusal survives the crossing.** `cross/odd` submits with **no
  transport registered**, so `NSF_EINVAL` is proved on the **Phase-1 drainer
  path**, not across the modelled crossing. Proportionate *because the decision
  produced one path* — `nsfsel_dispatch` is the same code either way — but said
  here rather than left for a reader to assume both.
- **That the two load modules agree on `sizeof(NSFSELITEM)`.** The host links
  facade and dispatcher into one compilation, so the multiply and the divide
  cancel *whatever* the size is. That agreement is the size assert's sole job,
  and it is a compile-time check on each side separately — no host run tests it.
- **The `test/mvs/tstd1b.c` edit.** Its two hand-built `ulen` values were not
  multiples of 8 and would now be refused, so they move in this change; the file
  is `host = false`, so that edit is **unverified here** and gets its proof in
  Stage 2. Stated as the honest division, not as a gap.
- **Anything about the `g_busy` wedge** beyond its cause. It is a consequence and
  goes away with the cause; the serial-service property it exposed is
  ADR-0042 §10's and (e) measures it.

---

## The audit was swept beyond `src/`, because a stale test site would be invisible

The write-side/read-side audit covers `src/` and `include/`. That is not enough
on its own: an MVS-only test (`host = false`) is **compiled but never run** by
any host gate, and `r.ulen = 2u` compiles perfectly — so a stale count-valued
`ulen` there would survive every check in this round and surface in Stage 2 as
an arm failing for a reason unrelated to what Stage 2 measures. That is the
hazard the `tstd1b.c` edit exists to avoid, and it has to be swept rather than
inferred.

`grep -rn '\bulen\b' test/ samples/` — every other site is already a **byte**
length (TCP/UDP send/recv buffers in `tsttcp.c`, `tstloss.c`, `tstudp.c`,
`tsttcpd.c`, `tsttcpr.c`, `tstudpm.c`; XFER staging in `tstubuf.c`, `tstxfw.c`,
`tstrqxf.c`, `tstrqxc.c`, `tstd1r.c`; `tstreqx.c`'s crossing vectors). None is
an element count.

`grep -rln 'RQ_SELECT' test/ samples/ src/` returns four test files, all
classified:

| file | routes via | state |
|---|---|---|
| `test/tstsel.c` | hand-built RQE | **edited** (6 sites) |
| `test/host/tsteza.c` | facade + hand-built controls | the gate |
| `test/mvs/tstd1b.c` | hand-built RQE | **edited** (2 sites), `host = false` |
| `test/tstreq.c` | `rqe_init(…, RQ_SELECT, 0u, 0u)` | **unaffected** — it only proves EOPNOTSUPP with no engine registered; `ulen` is never set |

`test/mvs/tstezat.c` (the M4-5 live SELECT gate) calls **`nsf_select`**, so it
moves with the facade and needs no edit; `test/mvs/tstd1a.c` has no SELECT at
all. The EZASOH03 `SELE` decoder path also ends in `return nsf_select(...)`
(`src/nsfeza.c:717`) rather than building its own RQE, so it moves too — which
is why `r.ulen` for SELECT appears exactly **once** in the whole tree.

---

## The wedge claim: a sound deduction written in the mood of a measurement

The §7 paragraph originally stated the block-forever wedge as fact. **It did not
start there.** The design memo states it, and the kickoff carried it forward:

> *"A cross-AS `select()` with no timeout wedges the stack for everyone, with no
> abend, no message and no trace."*

Every hop was checked in source, and every hop holds: no timer on the
block-forever form; `sel_scan` resolving nothing; `sel_finish` never running;
`g_priv.ecb` never posted; `g_busy` cleared only at `src/nsfsx.c:1232` under
`POSTED`. **The conclusion has never been run.** The wedge is observed neither
happening nor gone.

It is therefore re-cast as **the written prediction for Stage 2 arm 3**, recorded
here before that run so it cannot be edited afterwards:

> **Prediction (Stage 2, arm 3).** On the *unfixed* module, a cross-AS
> block-forever SELECT from client A leaves `g_busy` set for the life of the STC,
> and a subsequent request from client B is never served. On the *fixed* module,
> B is served. Falsified if B is served in both states (the wedge was never
> reachable), or served in neither (something other than `g_busy` is holding it).

**The rule this adds** — its home in CLAUDE.md §8.5 or elsewhere is Mike's call,
so it is recorded here rather than promoted:

> **A chain read out of source is a prediction until a run, however many of its
> hops were verified.**

This is a different failure from the four earlier prescription errors in this
round's lineage. Those were an unchecked property or an assumed mechanism — each
had a wrong hop. This one has **no wrong hop**: the reasoning is sound and it is
still not evidence. **The tell is the tense of the sentence, not the quality of
the reasoning** — "wedges the stack" reads as a measurement and was a deduction.

---

## One correction to the kickoff, carried into ADR-0047

The kickoff's read-side audit read:

> `nsftcp.c:601`, `nsftcp.c:629`, `nsfudp.c:199`, `nsfudp.c:268` each read ≤ `ulen` bytes

There is a **third** `r->ulen` read in `nsftcp.c`, at **`:2014`** (`tcp_send`);
`:601` is `tcp_send_resume`, the *parked* path. All three are byte counts and
correct, so **no conclusion changes** — but an audit is only worth having if it
is complete, so ADR-0047 §4 records **six** read sites, verified by grep.

The kickoff also named `docs/socket-provider.md` as carrying the `selectex()`
note. **No such file exists**; the note lives in
`docs/nsf370-provider-contract.md` §4, which is where `selectex()` is actually
discussed.
