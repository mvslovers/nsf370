# d1 §2.3 arms 1 and 2, re-run with the stimulus in evidence — predictions

Written 2026-09-15 against `main` at `6bcf447`, **before the test edit was
written**, before any deploy and before any job was submitted. **Not edited
afterwards.**

This is a **d1 round**, not (e). #107 assigned it there. M5-2 is flipped and
this discharges one of its two named unproven properties.

---

## 0. TWO SOURCE FINDINGS, BEFORE ANY CODE — one of them amends the kickoff's own discard rule

Both were read out of source while designing, and both change the round rather
than decorate it. They are stated here, up front, because **a rule discovered
as it fires is not a rule** — that is last round's lesson (#119), and this time
the constraint was found first.

### 0.1 A passive child TAKES A SOCKET SLOT, so the stimulus cannot come first

`tcp_child_create` (`src/nsftcp.c:1397`) calls **`soc_create`** for every
incoming connection and stamps `cs->apptok = ls->apptok`. So each held-open
connection consumes a table index.

`role_b` derives A's descriptor as **`own - 1`** (its own index minus one, the
#106 repair of defect 3). That holds only while A and B are adjacent in the
table. **A connect made before B allocates its socket inserts a child between
them** — A 0, child 1, B 2 — and `own - 1` would then name **the child**, not
A's listener.

It would have gone green. The child is also foreign to B, so every refusal
assertion would still pass — while the round silently tested the wrong socket,
and against a **non-listener**, for which `tcp_poll` reports not-ready *to its
owner too*. Arm 1 would have been vacuous in a brand-new way.

**Therefore the stimulus is fired AFTER B's sweep and socket creation**, on a
marker B emits itself. Sequence: A listens (idx 0) → B sweeps, creates its
socket (idx 1), derives `00010000` → **B announces** → the host connects (child
becomes idx 2) → A observes ready → B runs arms 1 and 2. **`role_b`'s
derivation and every one of its assertions are untouched;** only the window
moves, which is what the kickoff asked for.

### 0.2 "A reports ready INSIDE the arm's window" is UNSATISFIABLE — the rule is a BRACKET

The kickoff requires that an arm's window contain an A-ready line, and that an
arm without one be discarded. **That cannot be met, for two independent
reasons, and neither is a timing problem that better scheduling would fix:**

1. **Arm 2's window is exactly when A cannot answer.** B's parked SELECT holds
   `g_busy` for the whole park — service is serialised (ADR-0042 §10) — so a
   cross-AS poll A issues inside the window is dispatched only *after* it. This
   is **measured, not merely read**: #101 Stage 2 recorded V unserved while W
   was parked, with `SERVED` frozen 4 → 4.
2. **Arm 1's window is a single call.** No line can fall "inside" an instant.

**The evidential form is therefore a BRACKET, and it is sound because the
stimulus is MONOTONE:**

> A reports **not ready** before the connect; A reports **ready** at a poll
> **before** the arm; A reports **ready** at a poll **after** the arm; and
> nothing in between can have cleared it.

Nothing can clear it because A's `acceptq` is never drained: **A never
accepts** (accepting would remove the very stimulus), **B cannot accept A's
socket** — the ownership check under test is itself that guarantee — and A does
not close until after the final poll. `tcp_poll` (`src/nsftcp.c:2142-2147`)
reports a listener read-ready on a non-empty `acceptq`.

**Validity conditions, named so they are checked and not assumed:**

- **The host HOLDS the connections open.** A FIN is tolerable — the child goes
  CLOSE_WAIT but stays on the `acceptq` and the listener stays read-ready — but
  an **RST dequeues** it (`tcp_do_reset` → end of life). So: hold open, and
  **`tcpdump` showing no RST toward port 3011 inside the bracket** is the
  paired control for the monotonicity claim.
- **Backlog.** A listens with 5, capped at `NSFSOC_ACCEPTQ_MAX` 8, so at most 5
  pending. The round uses **three** connects: one before the arms, two in the
  park.

**A not-ready → ready transition in A's own spool IS the stimulus arriving, in
evidence.** The pre-connect not-ready lines are the instrument's own negative
control: they show the poll can say *no*, so a later *yes* is a reading and not
a constant.

---

## 1. P0 — the deploy is seen to take effect, and the fingerprint is stated

**`NSF.LINKLIB` is NOT redeployed and does not need to be:** `src/`, `asm/` and
`include/` are untouched, and `make test-mvs` targets **TESTLIB**, which no STC
holds. The `P NSFS` / `S NSFS` cycle is done for a different reason — §5's
generation-window condition, which arm 2.2's `RANGE INADEQUATE` guard depends
on.

**The fingerprint is a value the old TESTLIB cannot produce:** `role_a` now
emits **`TSTD1B: A POLL ... READY=...`** console lines. No previous build of
this module has ever written one. Absence of the member is loud in the other
direction (`IEA703I 806-4`), and the no-PARM stage run returns the existing
`NO ROLE -- SKIPPED` / **CC 20** signature.

**Both §5 tells are checked and both are stated as inapplicable here**, rather
than quoted as if they carried: there is **one** build of the changed module,
so *identical values across supposedly different builds* has nothing to compare;
and the inverse tell — *a value neither pure build can produce* — needs two pure
builds to enumerate error sets across. The new WTO line is the positive check
that replaces them.

---

## 2. P1 — arm 1, the poll path

With A ready and **reporting it on both sides of the arm**, B's poll returns
with the **foreign entry NOT ready** (`it[0].ready == 0`), **B's own entry
SERVED** (`it[1].ready != 0`, the in-arm control), and **no error**
(`r.errno_ == 0`).

**Falsified** if the foreign entry reads ready; or if the bracket is not in
evidence — which is **"the arm did not run"**, not a result, and is reported as
a discard.

*Clause checked.* The way a green arm 1 could be hollow is A not actually being
ready, which is precisely what the bracket exists to exclude — and the
not-ready lines before the connect are what stop "ready" being the only thing
the instrument can say. A second hollowness — testing the wrong descriptor — is
closed by §0.1.

## 3. P2 — arm 2, the parked path, and the EDGE is the point

B parks on A's descriptor alone. **While B is parked, two further connects
complete**, each firing `soc_notify_ready(ls, SEL_READ)` — unconditional per
graduation, `src/nsftcp.c:1153` — which makes `nsfsel_on_notify` **re-scan every
parked SELECT**, B's included, with **B's** identity taken from the SELCB.
`sel_scan` must skip the foreign entry, so B's SELECT does **not** complete and
times out `rc = 0` with `ready = 0`.

**That the edge is deliverable during the park is established, not hoped:** the
executive processes device input while `g_busy` is held (#101 Stage 2 measured
`EVTPASSES` 616 → 784 with `SERVED` frozen), so the 3WHS completes and the poke
fires even though no cross-AS request can be dispatched.

**This is the stronger of two readings and the round should get both.** A
*persistent* readiness (the first connect) tests only that park-time `sel_scan`
skipped the entry; the **edge** tests `nsfsel_on_notify`'s re-scan — the path
the #107 annotation says was never driven.

**Falsified** if B's SELECT completes (`rc != 0`) or the entry reads ready; or
if no connect is shown landing inside the park window — again a discard, not a
result.

*Clause checked.* The dangerous green here is a park in which **no poke ever
arrived**, which is indistinguishable from correct filtering. The tcpdump
capture, on the host's own clock, is what separates them, and the park is
widened to **20 s** (from 8) purely to make the edge land — it is the **window**,
not an assertion, and this round is one-shot.

**No in-run "a poke CAN complete a parked SELECT for its owner" control is
built.** That positive control already exists on this stand and this code path:
#101 Stage 2's role W completed `RC=1 MASK=00000001` on a host connect. Building
it again would bolt a wire-dependent arm onto `role_b` for no evidentiary gain.

## 4. P3 — A's own result is read, and A's arm can SKIP

`role_a` gains two assertions — A saw itself ready at least once, and A was
still ready at its final poll — **beside** #106's refusal assertions, which are
untouched.

**If A never sees itself ready the round returns CC 20 (skip), not CC 1.** The
stimulus is the stand's, not the product's — the `TSTRQX2` no-interface idiom.
A's condition code is recorded **beside B's**: a job whose result nobody reads
is §8.5 one level out, which is the defect #106 repaired one layer up.

## 5. P4 — the stand

`LARGEST FREE BLOCK` recorded at entry and exit. **This is a GATE, not a
measurement**, so the marker does **anchor detection only** — free storage is
not an input to any assertion here (#119's correction; for a measurement the
comparability argument would hold and the round would stop). The entry reading
has moved three times on this stand (**#120**) and no absolute value is treated
as expected. **An `NSF054W` discards the run.** Zero dumps, with the greps
paired against known-present lines.

## 6. What this round does NOT establish

- **Anything about (e)** or any throughput figure. No timing is reported.
- **d2**, the other named-unproven property. Untouched.
- **That a poke completes a parked SELECT for its OWNER** — cited from #101
  Stage 2, not re-run here.
- **Concurrent service.** The model is unchanged and serial; §0.2 depends on it
  being serial.
- **A milestone flip.** Nothing flips to proven; M5-2 is already flipped and
  this discharges a property it names.
