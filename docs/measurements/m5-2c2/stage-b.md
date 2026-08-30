# M5-2c2 stage b — the `ORPHAN` verb retired, its two words reserved

Implements the decisions locked on the stage-a map (PR #90). **Nothing is closed**:
obligation #4 is discharged **in substance for the identity half only**, #67 is **narrowed
not closed**, and the `TSTDEATH` restructuring is **reported and left open** (§2).

---

## 1. What changed

**The verb is rejected, not merely deleted — and the placement is the finding.**

Deleting the staging-dispatch test alone would have been wrong. That dispatch is a
fall-through chain (`XFER? … RQE? … else ECHO`), so an `FNORPH` request with the test gone
would have fallen through to the **ECHO default and been serviced as an ordinary ECHO** — a
retired verb quietly doing something. Instead `FNORPH` is rejected **by name, first in the
pre-claim chain**, ahead of the slot claim:

```
         L     R3,REQFUNC(,R8)    request function
         C     R3,=A(FNORPH)      retired -> reject, claim nothing
         BE    BADFUNC
         C     R3,=A(FNQUERY)     ... the probe verbs that name a slot
```

So a retired verb costs **no slot and no in-flight count, by position rather than by
argument** — which is also precisely what narrows #67 (§4).

**The rejection writes the caller's block.** `BADFUNC` mirrors `BADANC` (`LA R15,rc` /
`ST R15,REQRC(,R8)` / `BR R14`) rather than reusing `BADREQ`, which returns rc **in R15
only**. `BADREQ` is right for a request whose pointer cannot be trusted; here the eyecatcher
has been checked. Had it been used, the client would have read back whatever it initialised
`rc` to — `-1` in `tstdeath.c` — which is **indistinguishable from "the SVC never ran"**
(CLAUDE.md §8.5). A client must be able to *see* the rejection, and it now does: `rc = 4`.

**Removed:** the `ORPHIN` staging block, its dispatch test, the `PSTOK` no-WAIT test and the
`ORPHRET` return block. `asm/nsfvsvc.asm` 1289 → 1245 lines.

**Kept, reserved:** `pascb`/`pasid` → `rsvd_pascb`/`rsvd_pasid`, **same offsets**. Proven,
not asserted — every `NSFV_OFF_ASSERT` value and `NSF_SIZE_ASSERT(NSFV_REQ, 64)` compared
against `main` and **identical**; `NSFV_ANCHOR_VER` still 3.

### `NSFV_REQ_ORPHAN`: keeping it is not neutral — the cost, both ways

The kickoff asks what removing versus reserving the constant costs. Three options:

| option | cost |
|---|---|
| **delete it** | breaks `tstdeath.c`'s compile, which **forces the §2 restructuring decision now** rather than leaving it open — the one thing this round is told not to pre-empt |
| **rename** `…_RETIRED` | same compile break, same forcing effect, for a cosmetic gain |
| **keep the name, mark it retired** *(chosen)* | the header still *names* a verb that no longer works, so a reader who stops at the header is misled — mitigated by the comment at the definition and in the verb list, both saying retired and pointing at the rejection |

Chosen because it is the only one that does not decide §2 as a side effect. The honest cost
is the misleading name; it is paid down by the comment, and it disappears when §2 is settled.

---

## 2. `TSTDEATH` — options priced, nothing restructured

Its four scenarios lose their driver. Rows 1–2 have a real replacement (the `TSTAPPDS` rig,
6 of 6 on the transport path in stage a); rows 3–4 do not, and stage a established they were
never live-coverable in any useful sense.

**The structural problem is not test count.** `TSTDEATH` is an **NSFV** client; the
replacement induction drives **NSFS**. The M5-2c memo protected the Stage-0 set's isolation
explicitly, and the reason is diagnostic: **an isolated probe going red names a mechanism; a
socket operation going red names nothing.** Every option below is priced against that.

| option | what it costs the Stage-0 set's isolation |
|---|---|
| **A. Rewrite `TSTDEATH` against NSFS** | **Loses the isolation outright.** The DEAD path would then only ever be exercised through a socket op (bind/recvfrom) on the production STC, so a red run no longer says "the guard's wiring broke" — it says "something in the stack broke". It also makes a Stage-0 test depend on CTCI being up, which no Stage-0 test does today. Cheapest to write, most expensive diagnostically. |
| **B. Keep it on NSFV, rows 1–2 only** | **Not possible as stated, and the reason is the round's own result.** Rows 1–2 on NSFV require a client whose *address space* dies while NSFV holds its request. `TSTDEATH` is a batch job in an initiator — stage a measured that an ended batch client reads **LIVE forever**. Producing a real death against NSFV needs an STC client, i.e. an NSFV analogue of `TSTAPPDS` plus operator timing, which is new induction machinery and not deterministic in a batch gate. Row 1 alone *is* keepable — scenario 5 (the blocking ECHO with the real identity) already does it and needs no `ORPHAN`. |
| **C. Retire `TSTDEATH`, carry row 1 in the existing Stage-0 clients** | **Isolation preserved, coverage honestly reduced.** Row 1's wiring is already exercised by every boundary-crossing test — the guard gates *every* reply POST, so `TSTSVC`/`TSTUBUF`/`TSTXFW` are row-1 witnesses and a false DEAD would hang them. Rows 2–4 keep only their host pinning in `TSTREQX`. Cost: the NSFV round loses a named probe, and the DEAD path's live coverage moves entirely to the NSFS-side `TSTAPPDS` rig, which is not a Stage-0 test. |
| **D. Keep it as-is, failing** *(status quo of this branch)* | Costs a permanently red test, which is the one thing worse than any of the above: a round that is expected to be red trains the reader to skip the matrix. **Not viable beyond this branch.** |

**Not decided here.** Worth noting for whoever does decide: B's row-1 remnant and C differ
less than they look, since scenario 5 is the row-1 witness in both.

---

## 3. Gate results

### Host — the classifier's logic is still pinned

`make test-host` **3342 PASS / 0 FAIL**, unchanged. `src/nsfreqx.c` and `test/tstreqx.c` are
**untouched by this branch** (`git diff` empty for both), so all four rows — and all four
`UNKNOWN` branches — remain host-pinned exactly as before. The retirement removes a *live
driver*, not any host coverage.

### Offline gates on the assembler

- `tools/check-card-columns.sh` **OK**, and it **caught one of my own cards at 72 bytes**
  mid-edit — the documented failure mode, found before the toolchain saw it.
- `as370 -a=` listing checked: `C R3,=A(FNORPH)` → `5930 64F8`, `BE BADFUNC` → `4780 64A4`,
  **base R6, not dropped to 0**, and `64A4` matches `BADFUNC` at `0004A4`.
- **All 1268 source cards present in the listing, byte-identical, in source order** — matched
  as an in-order *subsequence*, because macro expansion (`WAIT`, `CVT`) injects statements
  and listing statement numbers are therefore **not** 1:1 with source lines.
- **The check was verified to discriminate.** A deliberate 89-byte card on the `C R3,=A(FNORPH)`
  line produced: `check-card-columns` FAILED, **as370 rc=8** (cc370#84 — a swallowed
  *statement* stops the build), and the statement check named **both** the overlong card and
  the `BE BADFUNC` it ate.
- **R3 liveness** at the insertion point: `L R3,REQFUNC(,R8)` is immediately followed by the
  new compare, nothing between.
- **Fall-through**: `XFERIN` still ends `B DOPOST`, and between it and `RQEIN` there is
  **only comment text** — no orphaned code where `ORPHIN` was.

### Live — NSFV round, NSFS round, and `TSTDEATH` stated rather than dropped

Deployed to `NSF.LINKLIB` with no STC holding it (checked first) and no mid-chain 500.

| round | result |
|---|---|
| **NSFV** — `TSTSVC` / `TSTMVCK` / `TSTUBUF` / `TSTXFW` | **412 PASS / 0 FAIL**, all CC 0 batch+TSO |
| **NSFS** — `TSTRQXC` / `TSTRQXF` | **122 PASS / 0 FAIL**, CC 0 batch+TSO |
| **NSFS** — `TSTRQXM` | **batch CC 0**; host peer **9353 bytes byte-exact** |

**`TSTDEATH` is excluded and that is stated, not quiet.** The NSFV figure moves **484 → 412**
— exactly the 72 assertions `TSTDEATH` contributes. `TSTMVCD` stays excluded (#53).

`TSTRQXM`'s TSO arm FAILs **by design** — the one-shot listener is consumed by the batch run,
so `CONNECT` and its dependent `CLOSE` fail (`got -1`). Verified to be exactly that pair, not
something new. Batch is the gate (TSTTCPW precedent).

### The retired verb's rejection, from an unauthorised client

`TSTDEATH` is that client, so it doubles as the probe. It now reports **FAIL CC 1** — and
`CC 1` is "ran and failed", **not** the `CC 20` "did not run" idiom, which is the distinction
that keeps §8.5 honest here. Three assertions carry the result:

```
FAIL: LIVE orphan: SVC accepted and returned without waiting  (got 4, want 0)
FAIL: LIVE client: request SERVICED (state DONE), not reaped  (got 0, want 2)
FAIL: LIVE client: in-flight count NOT given back             (got 0, want 1)
```

`rc = 4` (`NSFV_RC_INVALID`) **written into the caller's block**; slot state **0 = FREE** so
nothing was claimed; `inflight = 0` so no count was taken. "Rejected ahead of the claim,
costs no slot and no in-flight count" — **observed, not argued**.

### The revert test — three states, one assertion moving

| state | `TSTDEATH` | rc from the retired verb | slot claimed | forged reaps on the console |
|---|---|---|---|---|
| **1 retired** | FAIL CC 1 (52 / 20) | **4** | no — FREE, inflight 0 | **0** |
| **2 reverted** (the three files back to `main`) | **ok CC 0** (72 / 0) | **0** | yes | **6** |
| **3 restored** | FAIL CC 1 (52 / 20) | **4** | no — FREE, inflight 0 | **0** |

**State 2 is rendered positively, not as an absence** — the forged identity is shown
*taking*, and each forged row is identifiable in the console:

```
NSFV050I CLIENT DEAD (ASCB=00FD0F18 ASID=0020) -- REQUEST REAPED   <- a free ASID nothing owns
NSFV050I CLIENT DEAD (ASCB=00FD0F20 ASID=0008) -- REQUEST REAPED   <- the real ASCB +8
NSFV051W CLIENT LIVENESS UNKNOWN (ASCB=00000000 ...)               <- the NULL, row 4a
```

None of those identities belongs to any address space that has ever existed on this stand.
States 1 and 3 are byte-identical in outcome, and state 3's working tree is `git diff`-clean
against the committed retirement.

**Zero dumps** across the round; both STCs start and stop clean; `SVC 239` stolen and
restored every cycle; no `NSF054W`; stand left with nothing running.

---

## 4. #67 — narrowed, not closed (proposed text, NOT applied)

Retiring `ORPHAN` removes the variant that needs **neither a parked task nor a client death**:
one live unauthorised task could repeat the call and strand a slot each time, because
`ORPHRET` returned without parking. That is gone — the verb is now rejected before the claim,
so it strands nothing.

**`ECHO` and `XFER` still strand a slot each.** The mechanism is unchanged and is the one the
issue states: `nsfsx_next_actionable` skips any slot that is not `PENDING`, and the
`ACT_DISPATCH` arm sets `HELD` for any staged `xfunc` other than `NSFV_REQ_RQE`. Nothing
re-examines a `HELD` slot. A parked task costs exactly one slot and hangs itself, so
exhausting the pool needs 64 of them.

**Posted to #67** (comment only — the issue stays OPEN, no state change):

> `ORPHAN` was retired in M5-2c2 stage b (PR #90's map priced it; the retirement is on
> `m5-2c2-orphan-retire`). It is now rejected by name in the pre-claim chain and returns
> `NSFV_RC_INVALID` **without claiming a slot or taking an in-flight count** — verified live:
> slot state `FREE`, `inflight 0`. So the sharp variant in this issue — one live, unauthorised
> task stranding slots by repetition, with no parked task and no death — is gone.
> **The issue stays open.** `ECHO` and `XFER` still strand a slot each by the same
> `HELD`-and-never-re-examined mechanism; each costs one parked task, so 64 parked tasks still
> exhaust the pool, and the CSA retain path still follows if the parked client's address space
> is gone. The remaining probe verbs are c3.

---

## 4a. The STATS truncation — filed as #92

`F NSFS,STATS` renders ~32 of 52 counters into a fixed 512-byte buffer with a **data-dependent
cut point**, and `src/nsfmsg_host.c`'s `CAP_MAX = 64` ring drops NEWER lines so the `NSF816I`
summary is evicted at a full registry. Filed as **#92**, with the (e) dependency stated and no
fix proposed: (e) measures throughput by reading counters, so a renderer that silently drops a
fifth of them at a moving boundary is a measurement instrument that fails silently.

## 5. Obligation #4 — half discharged, and it must not be booked as met

`ORPHAN` was **the one place a request-supplied identity was trusted verbatim**, and that is
gone. That is the **identity half**, discharged in substance.

**#4 is not met.** The rest of the probe scaffolding — `ECHO`, `XFER`, `UNSTAGE`, `SLOT`, and
`QUERY`'s promotion — is **c3, after (e)**. The scaffolding is still reachable from an
unauthorised caller, which is why the memo calls its removal a security item rather than
hygiene.
