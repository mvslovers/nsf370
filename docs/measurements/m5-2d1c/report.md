# d1c — the #67 rejection, and the encoding question settled

**Status: §1 DONE and offline-gated, its live gate built and NOT YET RUN.
§2.1's blocking question SETTLED offline. §2.1's code change, §2.2 and §2.3
NOT STARTED — they need PR #100, which is still open.**

Branch `m5-2d1c`, on `main` at `6046003`. Host **3469 PASS / 0 FAIL**, unchanged
— and that is a **no-regression check only**: every file this round changes is
MVS-only (`asm/nsfvsvc.asm` never compiles on host; `src/nsfv.c` and
`src/nsfsx.c` are Phase-2 STCs; `test/mvs/tstrqxf.c` is `host = false`).

---

## 1. The #67 rejection — DONE

### What it does

A probe verb reaching the **production** STC claims a CSA slot and an in-flight
count, is set `HELD` by the dispatch arm because its `xfunc` is not `RQE`, and is
never re-examined: the client parks on its reply ECB forever and the slot is gone
for the life of the STC. The SVC routine now refuses **ahead of the claim**, so
the refusal costs **no slot and no in-flight count by position rather than by
argument** — the form M5-2c2 established for the retired `FNORPH`. The rc goes
into the **caller's block** (`BADFUNC`, not `BADREQ`'s R15-only), so a client
cannot read back its own initialised value and take it for "the SVC never ran".

### The shape changed, and the reason is empirical

The kickoff said *refuse by name*. **Tracing the staging dispatch first showed
that would not have discharged the round's own justification.** `CLAIMOK`'s
dispatch is a fall-through chain:

```
L R3,REQFUNC(,R8)
C R3,=A(FNXFER)  -> XFERIN
C R3,=A(FNRQE)   -> RQEIN
[fall through]   -> ECHO staging
```

So **`ECHO` is not the only way in.** Any `REQFUNC` the routine does not
recognise — one wrong word in a client — falls through, stages as `ECHO`, and
hangs identically. Naming `ECHO` and `XFER` leaves that open, and it is the
*cheapest* instance of the fault: it needs no knowledge of the verb set at all.

**So the production STC permits exactly `FNRQE` and refuses everything else.**
One compare-branch pair instead of two, and it closes the whole class. Mike
ratified the change of shape before it was written.

`QUERY` / `UNSTAGE` / `SLOT` are unaffected either way: they branch out of the
chain **above** the insertion point and take no slot.

**Placement is LAST in the pre-claim chain, not first**, and the kickoff's
"first" is the *positional guarantee* (ahead of the claim ⇒ no slot, no count),
not the literal position. It has to follow the `QUERY`/`UNSTAGE`/`SLOT` branches
because those verbs are serviced at **both** servers. Said at the code, because a
reviewer reading "first" literally will flag it.

### The gate: an anchor capability bit

`NSFV_ANCHOR_PROBE` (`X'40000000'`) in the existing `flags` word. Set by the
probe STC (`src/nsfv.c`) and by nothing else; the production STC leaves it clear,
and **the omission is stated at that line**, because an omission is invisible.

* **Polarity is fail-closed.** The bit says *permitted*, never *forbidden*, so
  the zero a zeroed or unset anchor carries means REFUSE. A server that has not
  said it services probe verbs is not assumed to.
* **`flags`, not `rsvd0`.** `rsvd0` is the **last** slack word in the header and
  its own comment names the use it was reserved for (the lost-race counter);
  spending it here would put the next addition back to a full Stage-0 round.
  `ANCFLAG` is already loaded and `TM`'d one instruction above, so the marginal
  cost is one instruction.
* **`NSFV_ANCHOR_VER` stays 3, and neither skew direction is silent.** No field
  moved (the offset asserts are byte-identical to `main`). A stale router does
  not test the bit and services probe verbs as it does today — correct at NSFV,
  no worse than today at NSFS. A new router against a stale STC reads the bit
  clear at NSFV and refuses every probe verb there, taking the **whole Stage-0
  set red at once**. A version bump is the honest answer to a *silent* skew;
  there is none here.

### What it does NOT do

**It does not close #67.** `ECHO` and `XFER` still strand a slot each at the
probe STC, which services them. M5-2c3 retires the verbs and **deletes this block
with them** — said at the code, so it is not left as unattributable dead code.

The `HELD` arm in `src/nsfsx.c` stays as the backstop it always was. Making it
unreachable from a client is not the same as making it correct: a slot set `HELD`
there is still never re-examined.

### Offline gates

| gate | result |
|---|---|
| card columns | OK — every card inside column 71 |
| `as370` | rc 0, no diagnostics |
| statement presence | all **1368** source cards in the listing, in source order |
| `TM ANCFLAG(R2),ANCPROBE` | `9140 200C` — mask `40`, disp `00C`, **base R2 not dropped to 0** |
| `BO PROBEOK` | `4710 609C` — base R6, target `00009C` = `PROBEOK` |
| `C R3,=A(FNRQE)` | `5930 6540` — base R6; the literal at `000540` reads `00000006` |
| `BNE BADFUNC` | `4770 64DA` — base R6, target `0004DA` = `BADFUNC` |
| instruction stream | 364 → **368** emitted own statements, **delta exactly the four inserted**; 274 identical, **90 differing by a uniform displacement shift, 0 by anything else** |
| offset asserts | **byte-identical to `main`** — anchor layout unmoved |
| cross-build | 6 modules + 55 test modules, no warnings |
| alias scan | **248 unique, all ≤ 8**, **none added** |
| host suite | 3469 PASS / 0 FAIL (no-regression only) |

**All three assembler gates were verified to discriminate**, not assumed to: a
deliberate 74-byte comment on the `TM` line makes the card check FAIL, `as370`
report *"This card was consumed as a continuation and the statement on it
discarded"* at **severity 8**, and the statement check name **both** victims —
the overlong card and the `BO PROBEOK` it ate.

### The live gate — built, not yet run

`asm/*.asm` must be validated on MVS before it merges (CLAUDE.md 8.4), and §1 has
never run. Section **(E)** of `TSTRQXF`, the NSFS-side probe gate, drives two
requests:

* **`ECHO`** — the verb #67 names.
* **an unrecognised `REQFUNC`** — and this one is the point. A test driving only
  `ECHO` would pass against a refuse-by-name gate that leaves the fall-through
  open, so the gate would not discriminate between the two designs.

Each asserted three ways: the rc reaches the **caller's block** (initialised to
−1, so `rc == 4` also proves the block was written), `inflight` unchanged, and no
slot changed state — both counts read straight out of CSA before and after.

**Its failure mode is a HANG, not a failed assertion**, so each request is
bracketed by `wtof` markers: the console log survives the cancel a hang forces,
where buffered SYSPRINT does not (the M4-5 lesson, applied in advance).

**The "before" arm is on record from #100's live round** — a cancelled job, a
leaked slot, `NSF054W`, an anchor retained to IPL — and is **deliberately not
re-induced**, because inducing it costs exactly that again.

**The negative control is free and sits in the other half of the round.**
`TSTSVC` (ECHO) and `TSTUBUF`/`TSTXFW` (XFER) run against NSFV, where the bit
**is** set; those staying green is what makes this a **conditional** gate rather
than "the verbs are refused everywhere".

---

## 2. §2.1's blocking question — SETTLED

`NSFV_REQ_EYE` is **EBCDIC** (`D5 E2 C6 E5`), and #100's `4E 53 46 56` was
**derived from the C source, not observed** — the captured line stops after eight
bytes and never contains the `(wanted …)` segment at all. Full chain, both
corroborations and the four consequences for case 2 in
[`eyecatcher-encoding.md`](eyecatcher-encoding.md).

The kickoff made this a precondition for quoting any case-2 result. It is
discharged, and it was discharged **offline** — no stand time.

---

## 3. NOT STARTED, and why

**PR #100 is still open.** `test/mvs/tstd1r.c` exists only on
`m5-2d1-live-b`, and #100 also carries the repaired `tstd1b.c` that §2.2's
ownership arm must be measured with. Copying either onto this branch would
duplicate #100's diff into this PR — the trap #98's retarget check exists to
prevent — and merging is the maintainer's countersign, not mine.

* **§2.1's code change** — the minus-one staging and the diagnostic repair.
  Specified precisely in `eyecatcher-encoding.md` §"Consequences"; it is four
  edits to a file this branch does not have.
* **§2.2** — the three-state revert for both checks.
* **§2.3** — Phase 1 live.

All three are live work and would run in **one** round with §1's live gate, on
the sequence already written down in [`sequence.md`](sequence.md).

**The stand is up and reachable** (checked: `D T` answers, `mvsdev` up 24 days),
so the only thing missing is the merge.
