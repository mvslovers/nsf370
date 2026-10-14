# NSF — Assembler Rules: the Full Record

The complete text of the "C-callable HLASM" invariants, with the measurements
and incidents behind each rule. It used to sit in `CLAUDE.md` §3; it moved here
verbatim on 2026-10-14 so `CLAUDE.md` could stay within the size a session can
load. **`CLAUDE.md` §3 still states every rule** in short form and is the
review checklist; this file is why the rules are what they are. If the two ever
disagree, the disagreement is a defect — fix it in both.

---

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
  operands `D(B)`, never `D(,B)` (#5). Keep every statement inside **column 71**:
  as370 reads column 72 as a continuation flag, so a **comment that overruns
  column 71 on an INSTRUCTION line** makes as370 treat the next line as a
  continuation and **silently drop the operand — or the whole instruction**. The
  M3-0b S0C1 was exactly this: an over-long comment on the `SVC 33` line dropped
  the `SVC 33` itself (and the save-area restore), so `ctci_halt_read` returned
  through garbage and branched to low storage. **A green host build and a clean
  cc370/as370/ld370 link are NOT evidence** — the merge is invisible off-target;
  only the live ABEND (or the `as370 -a=` listing) shows it. Keep
  instruction-line comments short and within column 71.
  **CORRECTED 2026-08-28: a WHOLE-LINE `*` comment is NOT exempt, and the old
  advice to "put long rationale in a leading `*` comment block (those are
  full-width and safe)" was wrong — it is what produced the damage.** IFOX00
  applies the continuation rule to comment statements too (`PNXT13` reads them
  with `RALLCNT`), so a `*` card reaching column 72 **eats the next card**;
  as370 short-circuited comments before looking at column 72 and so disagreed
  with the target until mvslovers/cc370#72 (`IFO026`/`IFO069`, severity 4).
  Measured here on the fixed as370: `asm/nsfctcio.asm` line 95, a 74-byte
  comment reading "DSECTs before the code", swallowed the `DCBD DSORG=PS` on
  the next line, so **every** DCB DSECT symbol resolved to 0 —
  `MVC DCBDDNAM-IHADCB(8,R2),0(R3)` assembled as `MVC 0(8,R2),0(R3)` and
  `TM DCBOFLGS-IHADCB(R2),DCBOFOPN` as `TM 0(R2),X'00'`, a mask that can never
  be true. `asm/nsfvsvc.asm` lost **four** cards (646-byte object difference),
  and they are four separate safety mechanisms: `ANCVERNO EQU 3` (so the SVC
  routine's own anchor-version check compares against **0** — the stale-router
  defence), `ASCBASID EQU 36` (so it reads the caller ASID from **ASCB+0**, and
  ADR-0040's client-death guard classifies the wrong field), `L
  R3,REQFUNC(,R8)` (the request function is never loaded) and `DOUNSTG DS 0H`
  — which turns `BE DOUNSTG` into **`4780 0000`, a branch to address 0** in
  supervisor state key 0. **The rule is therefore: NO card of any kind, comment
  or instruction, may reach column 72.**
  **CHAINS ARE LONGER THAN ONE CARD, and a card-by-card reading misses them:**
  573 and 574 are BOTH overlong, so 573 continues onto 574 and 574 onto 575,
  the statement. Looking only at "which card does this one eat" stops at 574,
  sees a comment, and concludes nothing was lost. Read the assembler's own
  diagnostics instead of deriving the victims — **and check the tool is
  reporting all of them**: as370 truncated its diagnostic array at 128 entries,
  `nsfvsvc.asm` emits 130, and the two that carried the severity were the last
  two, so it reported 2 of 4 and the RC depended on what happened to fit
  (cc370#85). Twice in one day a silent cap hid a statement loss.
  **The object-comparison gate has ONE spurious byte, and it is a date:** the
  END record carries `ASM370 <ver> <julian>` and the Julian day sits at
  **column 52 of the last card**, so an object assembled on one day and
  re-assembled the next differs by exactly one byte (`F0`→`F1` at the 240→241
  rollover, measured). Same-day assembly is deterministic, which is why an
  earlier same-day control wrongly concluded there was no stamp at all. Mask
  that byte before reading a diff as a code change — all nine modules showed
  it at once, which is itself the tell.
  **The assembler now sorts the two cases, and the gate is back where it
  belongs (cc370#84).** What matters is the card that gets EATEN, not the one
  that eats: a swallowed **comment** costs nothing and stays IFOX's severity 4
  (survivable, e.g. a `TITLE` string running long); a swallowed **statement** is
  the code, and as370 raises it to **RC 8** so the build stops. mbt#94's
  "RC < 8 survives" is therefore safe, and mbt c80e23c adds
  `.DELETE_ON_ERROR:` — without it as370's deck outlived the failed build and
  the NEXT `make` linked it and returned 0, so the red build went green on the
  second run with the statement missing. Verified here: run 1 rc=2 and the
  object deleted, run 2 rc=2 again. Note IFOX does **not** warn at all when the
  eaten card's columns 1-15 are blank, so a macro operand card can still vanish
  quietly. `tools/check-card-columns.sh` (CI job `asm-cards`) remains worth
  having for a different reason: it runs before the toolchain, sees sources the
  assembler never gets, and also reports the ~20 harmless cards — a house-style
  call the assembler should not make for us. It counts BYTES, because a UTF-8
  `§` costs two and several cards measured 73 while looking like 72.
- **`as370` knows five instruction formats and three S-format instructions.** Its
  table is RR/RX/RS/SI/SS plus exactly `IPK`/`SPKA`/`STCK` — there is **no SSE and
  no RRE format at all**. Anything outside that set must be emitted as raw bytes
  (`DC X'…'`) with the encoding derived from **primary source** (the Hercules
  `opcode.c`/`control.c` for this target, not memory), and there are **two distinct
  failure modes with one rule**: a mnemonic as370 *knows* can assemble to silently
  **wrong** bytes — `MVCK` is in the table as plain `F_SS`, so it drops the `R1`
  length and `R3` key registers (hence raw `D9`, ≤255-byte pieces, ADR-0039) —
  while a mnemonic it does **not** know cannot assemble at all: `SSK`/`ISK` (raw
  RR `08`/`09`) and `MVCDK`/`MVCSK` (SSE `E50F`/`E50E`, and **absent on this
  target** — they take `S0C1`, ADR-0039). `LRA` (RX `B1`) *is* in the table.
  **The `as370 -a=` listing is the gate on the emitted bytes — never a green link.**
- **`SSK` sets the key on a REAL frame, and a pageable page moves underneath it.**
  `SSK`/`ISK` take a *real* address, so manufacturing a protected page means
  `LRA` the virtual page → `SSK` the real frame → touch the virtual page. If MVS
  steals and rebinds that page in between, **the key was set on a frame the
  program no longer has** — measured as three different real frames for the same
  virtual page across three runs. Used in `test/mvs/tstmvcd.c` 2.2b and
  `test/mvs/tstmvck.c` scenario 3. **The direction of the error is what decides
  whether it invalidates a conclusion, and it is one-directional:** both tests
  expect the access to **fault**, so a lost key means no fault means the
  assertion **FAILS** — verified against *every* `SSK`-dependent assertion in
  both files (2.2b's `ISK` read-back reads the *saved real address*, so it stays
  a true statement about that frame and cannot mask the store check;
  `tstmvck.c`'s `keyback` is logged, never asserted). So the technique yields
  **false negatives, never false positives**: a run in which the assertion
  PASSED is trustworthy, and ADR-0039's Stage-0b conclusion stands unchanged —
  do not reopen it on the strength of "the technique is flaky". Prefer a
  destination needing no `SSK` at all (a `GETMAIN SP=241` block is key 0 by
  allocation — `tstmvcd.c` 2.2a, which this hazard cannot touch) and keep the
  `SSK` variant only as a second, independent reading.
- **Exception:** a routine the OS invokes as an *exit* (not called from C) is not
  a C callee and does not get `FUNHEAD` — e.g. `NSFTMEXP`, the STIMER exit.
- **Reviewer checklist (assembler):** a new C-callable routine uses
  `FUNHEAD`/`FUNEXIT`, not hand-rolled; its entry name matches its `asm()` alias;
  data addressed by explicit displacement; `CS`/`LM` stay `D(B)`; nothing past
  column 71. A green host build and a clean link are **not** evidence — this
  failure is MVS-runtime only (issue #8).

