# ADR-0043 — The Phase-2 cross-address-space wake: one private ECB, consumed by the drain, with no floor under it

**Status:** Proposed (2026-08-27). Settles **how the NSFS executive learns that a client
published a request, and what it is entitled to assume between wakes.** The mechanism has
existed since M5-2a and the reset half since 64-1 (PR #72, merged); what did not exist is the
record. This ADR writes the contract down, at the strength four measurement rounds support and
no further.

**It changes no code.** `src/`, `asm/`, `include/` and `project.toml` are untouched by this
change; every behaviour described below is already in `main`.

**Relates to:** ADR-0022 (the reset-before-WAIT / double-check loop shape this contract's reset
half belongs to, and whose 64-1 annotation records the divergence and its cost), ADR-0034 (the
timer arming/consumption contract — *queue empty ⟺ STIMER disarmed*, which is why the startup
heartbeat stops for good the first time a real timer is cancelled), ADR-0038 (the private-SVC transport, the CSA anchor, `DOPOST`),
ADR-0040 (the client-death guard, which governs the **reply** POST and not the wake POST),
ADR-0041 and its 2026-08-23 addendum (the `NSFRQE` crossing; the decision that the wake ECB
lives in STC-private key-8 storage), ADR-0042 (the slot pool; §10's *one unit of work per pass*,
which bounds what a wake can be lost *for*), spec §5.3 (the executive loop and its ECBLIST).

**Evidence pins — every factual claim below cites one of these:**
`docs/nsf-64-0-measurements.md` (the POST lands; the spin and its cost),
`docs/nsf-64-0b-measurements.md` (a latched instance still stalls; the POST target read from
storage), `docs/nsf-64-0c-measurements.md` (what a committed WAIT looks like on this stand;
the executive is not waiting during a stall), `docs/nsf-64-0d-measurements.md` (the stall is a
stuck MVS swap-out), `docs/nsf-64-1-measurements.md` and
`docs/measurements/64-1/round-log.md` (the reset in three deployed states; the floor measured
and the service measured on the same floorless instance).
Source pins: `src/nsfsx.c` (`g_wake_ecb`, `nsfsx_drain` step 0, `nsfsx_pending`,
`nsfsx_next_actionable`), `src/nsfevt.c` (`evt_mainloop`'s ECBLIST and WAIT gate),
`src/nsfevt_plat.c` (`nsfevt_plat_wait`), `libc370/src/clib/@@ecbwl.c` (`WAIT ECBLIST=(2)`),
`asm/nsfvsvc.asm` (`DOPOST`), `asm/nsfstim.asm` (`NSFTMEXP`), `src/nsfsmain.c`
(`nsftmr_plat_arm(1u)`, `evt_set_request`).

---

## Context

Phase 2 put an address-space boundary between an application and the executive. Everything
about *crossing* it has an ADR — the transport (0038), the payload bounce (0039), the death
guard (0040), the request block (0041), the slot pool (0042). What none of them states is the
thing all five depend on: **the executive is asleep, and something has to wake it.**

For four rounds that was not merely unstated but unmeasurable. Issue #64 opened on the
hypothesis that the wake had "no floor"; 64-0 refuted the prediction built on it — the premise
was sound, the conclusion drawn from it was not — and found the opposite defect: the wake ECB
was never cleared, so the executive could not sleep at all. 64-0b, 64-0c and 64-0d then placed
the stall outside the wake path entirely, and 64-1 removed the spin. Only after that reset
landed could the question *"does the executive block, and does the POST wake it?"* be asked of
a running system at all.

Three things make writing it down now worth a document rather than a comment.

**It is measured rather than assumed.** Every clause below has a number behind it, taken on the
live stand, most of them in a controlled pair.

**A consumer already needs it.** `docs/nsf370-provider-contract.md` §4 records that a cross-AS
`selectex()` with an ECB list is *feasible* for NSF where it is meaningless for dyn75 — and it
is feasible for exactly the reasons this ADR states: a cross-AS branch-entry POST reaching an
ECB in the target's private key-8 storage, guarded on the reply side. That design (open
question 6 in the same document) cannot be written against an undocumented mechanism.

**And the failure mode this document exists to prevent is the one the investigation kept
walking into: reasoning about the wake from source instead of from measurement.** Two rounds
record a wrong turn of exactly that shape — 64-0's prediction, derived correctly from a true
premise and refuted by the first reading, and 64-0b's first draft, which concluded from a sound
middle step that the reset "cannot be the fix" — and a third records the same pull resisted
(64-0c §8, naming the inference it declined to draw). The fact that actually governs the idle
rate, and therefore what a wake measurement is measuring, is not in the C at all: it is in
`asm/nsfstim.asm`, and it has its own section below.

---

## Decision

### 1. One wake ECB, in the STC's own private key-8 storage, published in the anchor

`src/nsfsx.c` owns a single word, `g_wake_ecb`, in ordinary STC-private key-8 storage. Its
**address** is published in the CSA anchor at `server_ecb_ptr` (`ANCSEPTR` to the assembler)
and is what a client POSTs. `nsfsx_ecb()` hands the same address to `evt_set_request`, which
puts it in the executive's ECBLIST.

**That this is where the POST actually lands is measured, twice, by different means.** 64-0b
read the anchor from storage during a live round: the CSA fallback ECB at `anchor+X'10'` read
**0** throughout, while `server_ecb_ptr` at `+X'24'` read `000BCF94`, identical to `NSF041I`'s
own `ECB=`, with the target ASCB valid at ASID `X'000B'`
(`docs/nsf-64-0b-measurements.md` §3, fact 2). And independently, without reading the anchor at
all: `NSF812I` reports the **private word itself**, and it read `40000000`, which a POST
completion code can only reach if `DOPOST`'s `BNZ PSTECBX` branch was taken
(`docs/nsf-64-0-measurements.md` §4.2). **The fallback did not run.**

It is private and key 8 because the executive WAITs from **problem state, key 8**: a key-0 CSA
ECB in that ECBLIST is a documented abend (`S047` / `X'201'`), which is ADR-0041's addendum and
is not re-argued here. Publication order is that addendum's and is load-bearing for this
contract too: **the address is published before the SVC slot is stolen and invalidated after it
is restored**, so a client can never find an open transport together with an unpublished
address.

### 2. Two POSTs cross the boundary, in opposite directions, with different guards. Conflating them is the trap.

This is the distinction most likely to be got wrong by a reader — and it was got wrong once in
this ADR's own kickoff — so it is a table and not a sentence.

| | **the wake** | **the reply** |
|---|---|---|
| direction | client → STC | STC → client |
| issued by | the SVC routine, `DOPOST` in `asm/nsfvsvc.asm` | the executive, `nsfsx_drain` step 1 in `src/nsfsx.c` |
| mechanism | cross-AS branch-entry POST via `CVT0PT01`, target ASCB from `ANCSASCB` | `__xmpost(slot->req_ascb, &slot->reply_ecb, 0)` |
| target | `ANCSEPTR` — the STC-private key-8 word of §1 (else the fallback, §5) | the client's `reply_ecb`, in the CSA slot, key 0 |
| guarded by | the anchor's eyecatcher, `ANCVER`, the `ACTIVE` flag, and `PSTERR` on POST failure | **the ADR-0040 ASVT liveness check, immediately before the POST** |
| woken party runs in | problem state, key 8 | supervisor state, key 0 (the client is inside the SVC routine) |

**There is no ASVT lookup before the wake POST**, and there cannot usefully be one: ADR-0040's
asymmetry runs the other way, and ADR-0041's addendum records the mirrored STC-death race as
open residual risk rather than closed. The wake's own protection is the anchor validation
above, plus the in-flight count that keeps CSA alive while any client is inside the routine.

### 3. The wake is consumed by the drain, at its head, ahead of both scans

`nsfsx_drain` observes the POSTED bit (for the `wakeposts` counter), then **assigns
`g_wake_ecb = 0`, before either of its scans**. That position is the whole safety argument, and
it is short because the client's publish order makes it short:

> A client sets `req_state = PENDING` and only **then** POSTs. Relative to the reset there are
> exactly two orderings. A POST **before** it implies the publish was before it too, and both
> scans run after the reset, so the slot is seen on this very pass. A POST **after** it leaves
> the bit standing and the WAIT returns on it. There is no third ordering.

**Read the guarantee at its true scope.** The argument above is about the **bit**, not about the
**request**: what it establishes is that a client's POST always buys a pass in which that
client's slot is **looked at**. It does **not** establish that the slot is **served** on that
pass — because ADR-0042 §10 deliberately serialises service, and a second dispatchable
request while one is in flight is declined by design. So the guarantee this ADR asserts is:

> **No wake is lost for work the pass can consume.**

The deferred case is real, is deliberate, and is named in **Named gaps** (b).

**The reset is measured in three deployed states, with one assertion moving**
(`docs/nsf-64-1-measurements.md` §3): absent → `POSTED=Y`, 7 482 passes/s, 25.9–30.5 % of a host
core; present → `POSTED=N`, 9.98/s, 0.7–1.6 %; commented out, rebuilt and redeployed from source
verified **identical to `main` at instruction level** → `POSTED=Y`, 8 532/s, 26.0–26.9 %;
restored → `POSTED=N`, 9.99/s, 0.5–0.7 %. Service was unaffected in every state (8/8 inside one
console second). The deploy-took-effect observable is `POSTED=N` **with a non-zero `SERVED`** —
a combination that was impossible before the reset; its complement is ambiguous and was not used
as a check, which is why the reverted arm is corroborated by the instruction-level diff and the
deploy output instead.

Phase 1's `do/while` recheck loop is **deliberately not replicated**. The recheck half of
ADR-0022's discipline already exists one level up in the WAIT gate, which asks its question
through the same `nsfsx_next_actionable` the drain does; only the reset half was missing. The
three reasons a loop here would be a regression — a spin on the `__super` no-progress return,
no finiteness argument in Phase 2, and a silent conversion of ADR-0042 §10 into "serve every
inline-completing request per pass" — are ADR-0022's annotation and the call site's, and are
not re-argued here.

### 4. The executive is entitled to block indefinitely. NSF provides no floor under the wake, and adds none.

This is the decision one can disagree with, so it is stated as one.

**Liveness across the boundary is the POST.** The executive may sit in `WAIT` for an unbounded
time with no timer armed, no device traffic and no poll, and a single cross-address-space POST
is **sufficient** to bring it back. It is not *necessary* in the strict sense — the same ECBLIST
carries the timer, handoff, generic, device, cib and stop ECBs, and the drain runs on every pass
however that pass was caused, which is exactly what **Named gaps** (b) leans on. What the POST
is, is **the only wake source the client controls**. NSF does not maintain a periodic wake to
hedge it, and this ADR declines to add one.

It is a measurement, not a preference. On a post-TCP-workload instance with the timer queue
drained — where ADR-0034's *queue empty ⟺ STIMER disarmed* has taken the startup heartbeat away
for good, see *What a reader will otherwise measure* below — `EVTPASSES` moved
**5 854 → 5 855 across 259 s**: one pass in 259 seconds, 0.0039/s, host CPU 0.2–0.7 % user
(`docs/nsf-64-1-measurements.md` §4). A second, longer window
on the same build after 164 570 requests read **320 733 → 320 736 across 972 s** — three passes
(§6 of the same document). And **paired with service deliberately, because `EVTPASSES ≈ 0` read
alone looks like a dead executive**: on that same floorless instance eight cross-address-space
requests were submitted and **served and returned inside one console second**, CC 0000, 8/8,
`EVTPASSES` 5 855 → 5 872.

**And the POST wakes a WAIT the executive has already committed to** — which is the clause that
makes the rest of this decision usable rather than merely observed. `@@ECBWL` is
`WAIT ECBLIST=(2)` with no count operand, i.e. **count 1 over the whole list**
(`libc370/src/clib/@@ecbwl.c`), so the loop is genuinely suspended between passes, not polling;
and on the floorless instance above, where the only thing that can have woken anything is the
POST, eight requests were served inside one console second. The two halves are assembled rather
than read off one instrument, and **Named gaps** (e) says which half is which.

**Scope, stated because it is the whole reason this clause is safe to write:** *no floor is
needed* is measured **for the healthy case**, which is the only case it measures. It says
nothing about the stall (see *What this ADR deliberately does not decide*).

### 5. The CSA fallback exists for the probe STC, and is unreachable under NSFS while the transport is open

`DOPOST` tests `ANCSEPTR`; if it is zero it falls back to `ANCSECB`, the key-0 `server_ecb` in
the anchor itself. That arm is **for `nsfv.c`**, the Stage-0 probe STC, which publishes no
pointer and genuinely wants the CSA ECB it WAITs on in supervisor state
(`src/nsfv.c` — `ecblist[count++] = (unsigned *)&anchor->server_ecb`).

Under NSFS the fallback cannot be selected while the transport is open: `nsfsx_start` publishes
the pointer **before** stealing the SVC slot, and `nsfsx_stop` calls `nsfsx_svc_restore()`
**before** nulling it — so no new caller can enter the routine after the pointer is gone. What
is proven, and what is not, is **Named gaps** (a).

### 6. The word is `g_wake_ecb`. `g_wakeecb` is a different word.

`src/nsfevt.c` has `static NSFECB g_wakeecb` — the loop's own generic wake, posted by
`nsfevt_wake()` for a `dev_send` or a request submitted from outside a loop pass, cleared in
step 2 of every pass. `src/nsfsx.c` has `static NSFECB g_wake_ecb` — the transport's, the
subject of this ADR. They are one underscore and one grep apart, they sit in adjacent slots of
the same ECBLIST, and only one of them is what a client POSTs.

---

## Consequences

1. **A consumer may build on §1–§4.** A cross-AS `selectex()` with an ECB list
   (`docs/nsf370-provider-contract.md` §4 and open question 6) can rely on: a cross-AS POST
   reaching private key-8 storage in the target; the executive blocking indefinitely between
   wakes; and the reply direction being guarded by ADR-0040. It may **not** rely on anything in
   **Named gaps**.

2. **The ECBLIST is built once, before `for (;;)`, and this contract depends on it.**
   `evt_mainloop` fills `ecblist[]` at loop entry and never rebuilds it, so the address handed
   over by `evt_set_request` is stable for the loop's life. A future change that rebuilt the
   list per pass, or copied ECBs by value rather than by address, would break §3's reset
   argument **silently** — the POST would land in a word the WAIT no longer watches. Recorded
   as a dependency, not as a passing remark.

3. **`wakeposts` counts wake events from 64-1 onward, and is a lower bound on POSTs.** Two posts
   arriving between one observation and the next coalesce, exactly as they do for any ECB. At
   small counts it reads exactly: 8/8, 28/28, 36/36 against `SERVED`
   (`docs/nsf-64-1-measurements.md` §3). Under load it falls **below** it —
   **55 384 against `SERVED` 55 475**, 91 fewer, measured mid-campaign
   (`docs/measurements/64-1/round-log.md`, "Gate 2 campaign"), and 164 257 against 164 570 by
   its end (CLAUDE.md §7, M5 / 64-1). Reading it as a tally over-reads it.

4. **Every `WAKEPOSTS` figure in `docs/nsf-64-0*.md`, `-64-0b-` and `-64-0c-` was taken under
   different semantics and is not comparable.** Before the reset the counter latched and
   thereafter tracked `EVTPASSES` at a constant offset — measured at exactly 3 361 across four
   readings of one instance (`docs/nsf-64-0-measurements.md` §2). The `~8 500/s` rates recorded
   there are the latch, not wakes. The note lives at the counter's declaration in `src/nsfsx.c`,
   which is where such a reader looks; it is repeated here because a contract document is the
   other place.

5. **The wake path costs about two passes per request, not a spin.** Measured across the 64-1
   campaign: `EVTPASSES` 4 204 → 320 733 against `SERVED` 69 → 164 570 — **≈ 1.95 passes per
   request** (`docs/nsf-64-1-measurements.md` §6). Against the pre-reset instance's
   ~8 500 passes/s and 26 % of a host core, permanently, on every instance that had ever
   served one request
   (`docs/nsf-64-0-measurements.md` §2; ADR-0022's annotation).

6. **64-2 is closed as unnecessary, not deferred.** The step reserved on the plan — *"the floor,
   if 64-0/64-1 show one is still needed"* — is retired by §4: the wake works with no floor at
   all, measured at two idle scales, with service demonstrated on the same floorless instance in
   the same window. It is recorded here so that nobody reads the plan later and builds it.
   CLAUDE.md §7 carries the one-line note; this clause is the reasoning behind it.

---

## What this ADR deliberately does not decide

- **Issue #64 — the stall.** It is not the wake: during a stall the executive is not waiting
  at all (an ordinary PRB, `WCF=0`, `RBXWAIT` and `RBECBWT` clear —
  `docs/nsf-64-0c-measurements.md` §6), and its address space is stuck part-way through an MVS
  swap-out, in fields NSF does not write and cannot see (`docs/nsf-64-0d-measurements.md` §1).
  That is the whole of what this ADR says about it. The mechanism lives in those two records and
  in the issue itself — *"NSFS address space stalls mid-swap-out: tasks non-dispatchable while
  `OUCBQFL = 80`"* — which is open.
- **Whether NSF should mitigate an MVS condition at all**, and in what form (`SYSEVENT DONTSWAP`
  being the obvious candidate, and a privileged, contract-level change). That is the
  maintainer's decision and it is not taken here.
- **Concurrent service.** ADR-0042 §10's *one unit of work per pass* is unchanged. Curing the
  deferred-request latency of **Named gaps** (b) means concurrent service, not a louder probe
  or a different wake.
- **The mirrored STC-death race.** ADR-0041's addendum records it as open residual risk; §2's
  table states which guard each POST has and does not extend either.
- **The `owner_ascb` sweep**, the probe-scaffolding removal, and the rest of M5-2c/(d)'s
  inherited obligations. None of them changes the wake.

---

## Alternatives considered

**Add a periodic floor as defence-in-depth — rejected, and the rejection is empirical rather
than aesthetic.** The argument for one is that a wake which is *required and sufficient* is a
single point of failure: one lost POST and the executive sleeps until something unrelated
happens. Three things weigh against it.

It would be a fix for a defect nobody has measured — §4's floorless instance served its eight
requests inside one console second — and every stall on record happened on a **spinning**
instance, which is the state in which the executive had the strongest floor of all.

It costs ~10 timer interrupts per second in exactly the state that currently has none: the
post-workload idle §4 measures. That re-trades the "an idle stack takes zero timer interrupts"
property ADR-0011 and spec §6.3 state, on an address space whose design point is to be idle
most of the time.

And it would **re-destroy the stall detector**. `docs/nsf-64-1-measurements.md` §2 records that
the reset already destroyed one detector: with the spin gone, a *correctly idle* executive reads
`ASCBEJST` bit-identical, exactly as a stalled one does, and the criterion had to be rebuilt as a
conjunction (EJST flat **and** a slot `PENDING` **and** `served` frozen). A floor puts a second
signal back on top of the thing under investigation. That investigation is open, and blinding
its instrument to buy insurance against an unmeasured failure is the wrong trade while it is.

If a floor is ever wanted, the cheapest form already exists and is deliberately kept for a
different reason: `nsfsmain.c`'s `nsftmr_plat_arm(1u)`, in *What a reader will otherwise
measure* below. Turning it into a floor means making it survive an empty timer queue, which is
an ADR-0034 change and belongs there.

**Keep the pre-64-1 behaviour and let the ECB latch — rejected**, and this is the one alternative
that was actually running in production. It delivers liveness trivially (the loop never sleeps)
at 26 % of a host core forever. ADR-0022's annotation carries the measurement and the
correction; it is listed here only so that the option is not mistaken for a design that was never
on the table.

**Post the CSA `server_ecb` for NSFS too, and drop the private word — rejected** by ADR-0041's
addendum: the executive WAITs from problem state, key 8, where a key-0 CSA ECB in the ECBLIST is
a documented abend. Not re-opened.

---

## What a reader will otherwise measure — the floor that is an accident of startup

A fresh NSFS instance carries a **~10 Hz wake**, measured repeatedly and consistently: 9.95/s
(`docs/nsf-64-0-measurements.md` §2), 9.97/s and 9.99/s (`-64-0b-` §5), 9.98/s and 9.99/s
(`-64-1-` §3). It is not part of this contract, and it is not designed as a floor.

It is `src/nsfsmain.c`'s `nsftmr_plat_arm(1u)` — one arm, for one tick, for operator liveness
while the timer queue is empty — kept self-sustaining by a fact that is **not visible in the C
at all**: `asm/nsfstim.asm`'s `NSFTMEXP` POSTs the timer ECB and then **re-arms `STIMER` for the
same interval** before returning. One arm at startup therefore yields a heartbeat forever, on a
loop nobody re-arms.

**ADR-0034's invariant removes it permanently the first time a real timer is used and
cancelled.** `tmr_cancel` emptying the queue calls `tmr_disarm` → `TTIMER CANCEL`, and nothing
re-arms it afterwards, because the self-re-arming chain has been broken and `nsfsmain` arms only
once at startup. That is why **a fresh instance and one that has run TCP behave differently**,
and why any measurement of the wake taken on a fresh instance is measuring the timer.

**This is the reason the wake could not be measured for four rounds.** On a fresh instance the
loop wakes ten times a second whatever the transport does, so "the executive is alive" proves
nothing about the POST; and until 64-1 the alternative state was a full-core spin, which proves
even less. The floorless post-workload instance is the only configuration in which the wake is
the only thing that can have woken anything — and it is not the one a reader reaches for.

It is also this ADR's own lesson, and it is narrower than "read the code": **the fact that
governs the idle rate is in the assembler, and the fact that decides what `WAIT` does is in a
macro expansion.** `libc370/src/clib/@@ecbwl.c` is `WAIT ECBLIST=(2)` with no count operand —
count 1 over the whole list — which is why 64-0c's blocking reference reads `RBECBWT` set
(*wait for fewer events than the number waiting*) alongside `RBXWAIT`
(`docs/nsf-64-0c-measurements.md` §5). Neither fact is discoverable in `nsfsx.c`, and both were
needed to interpret a reading.

---

## The traps, recorded because they cost this investigation four rounds

**1. The RB-address remnant. Test the POSTED bit, never a non-zero test.** A satisfied multi-ECB
WAIT leaves `X'80……'` in the ECBs that were **not** posted. 64-0 measured `009DCD10` in an
un-posted `g_wake_ecb` on two fresh instances (`docs/nsf-64-0-measurements.md` §2): a non-zero
test would have reported POSTED for an ECB that had never been posted, and confirmed the exact
opposite of the truth, in the one place where it would have been believed. CLAUDE.md §4 states
the rule; this is the sighting that earned it in the wake path.

**2. `wakeposts` changed meaning at 64-1.** Consequence 4 above. Recorded twice on purpose.

**3. Reading the C and not the assembler.** The `STIMER` re-arm (above) is in
`asm/nsfstim.asm`; the WAIT's semantics are in `@@ECBWL`'s macro expansion. Both are one file
away from anything a reader of `src/nsfsx.c` would open.

**4. A `/.dm` read of a word in NSFS's private storage returns HTTPD's bytes.** The httpd display
modules run in HTTPD's address space and LSQA is aliased at the same virtual address in every
address space, so `g_wake_ecb` at virtual `0BCF94` reads as something meaningless and completely
healthy-looking (`docs/nsf-64-0c-measurements.md` §2 — it read `ABCA0A9A` on one pass). The wake
ECB is **private storage by design (§1)**, so this trap is permanent for anyone instrumenting
this contract from outside the STC. The path that works is `ASCBSTOR` → the DAT-translated real
address, read through Hercules `r`; or `NSF812I`, which the STC renders itself.

---

## Named gaps — what this contract does NOT guarantee

**(a) The CSA fallback arm is proven for the probe STC and not for NSFS — where it is
unreachable by construction, with one narrow window that is reasoned rather than measured.**
What is proven: the fallback arm **is** exercised at scale, by the probe STC — ADR-0041's own
correction records that all of the Stage-0 assertions (**484 PASS**, `TSTSVC` / `TSTMVCK` /
`TSTUBUF` / `TSTDEATH` / `TSTXFW`, `docs/nsf-64-1-measurements.md` §8) take it, because `nsfv.c`
publishes no `server_ecb_ptr`; and since that loop has no floor of its own, only that POST can
have woken it. What is **not** proven, and is reasoned: the narrow shutdown window in which a
client is already inside the routine when `nsfsx_stop` nulls the pointer, loads `ANCSEPTR` as
zero, and POSTs a word that is not in the executive's ECBLIST. It should self-drain — `ACTIVE`
is clear by then, so the nudge from `nsfsx_drain_inflight` sends the client down `WQUIES` — but
that nudge loop is itself live-exercised with **exactly one** parked client (CLAUDE.md §7, M5-2b4:
reaching more than one is not proven). Reasoned, not measured, and stated as such.

**(b) A dispatchable second request while one is in service is deferred, and its wake is
consumed.** `nsfsx_pending` reports **not-pending** for it — `nsfreqx_actionable(DISPATCH, busy)`
returns 0 — so the executive commits to the WAIT with that request outstanding and its POSTED bit
already cleared by §3's reset. It then waits for an unrelated wake. This is **deliberate**: the
alternative is a probe that answers yes to work the drain will decline, which is a hot spin on
the executive task (the anti-spin rule at `nsfsx_pending`'s header and in
`src/nsfreqx.c`). It is also **pre-existing and known** — b3 named it, and it is what the stall
issue's original title described, before that title was measured to be wrong and changed.
Before 64-1 the ~10 Hz heartbeat covered it; on a post-TCP-workload instance §4 measures that
there is no heartbeat at all. Curing it means concurrent service. The
delay case is **reasoned from the code path, not measured**: the 64-1 campaign's refusals were
pool exhaustion (`EXHAUSTED` 10 364) rather than this, and its verbs were largely
inline-completing.

**(c) Client death in the window between the ADR-0040 guard and the reply POST is not closed.**
What is proven is the guard itself: the ASVT classifier runs immediately before every reply POST,
its arithmetic is host-pinned (TSTREQX, all 60 rows), and both DEAD and UNKNOWN rows have been
seen live against the production STC (`NSF050I` twice, `NSF051W` once — CLAUDE.md §7, M5-2b3).
What is not proven is the interval between the check and the `__xmpost`. ADR-0040 §7 records the
same shape honestly; nothing here narrows it.

**(d) Two clients in two address spaces POSTing concurrently — what b4 proved and what it did
not.** Proved: two address spaces drove the transport simultaneously and both were served, with
contention measured from both ends — `collisions` 0 → 150 against a SOLO negative control of
0 → 0 on the same instance, A served 239 / refused 2 761 and B served 194 / refused 154, and
`exhausted` moving by exactly 2 761 + 154 = 2 915 (CLAUDE.md §7, M5-2b4). Extended by the 64-1
campaign to **164 570 requests across 45 rounds with no stall and no lost service** — 45 × B
CC 0000 and 42 × A CC 0000, the three CC 0001s being a gate-internal timing assertion, not an
NSF defect (`docs/nsf-64-1-measurements.md` §6). **Not**
proved: that no wake was ever lost — `wakeposts` is a lower bound (Consequence 3) and cannot
distinguish a coalesced POST from a dropped one; what stands in for it is that every one of those
164 570 requests was served at ≈ 1.95 passes each. Also not proved: hardware arbitration of a
simultaneous `CS` (CLAUDE.md §7, M5-2b4: phase 1 is positive evidence that no lost race occurred
*in that phase*, so on this stand it is unobserved rather than merely unmeasured), and
concurrent *service*, which is out of scope by **Named gaps** (b).

**(e) The executive was blocked in the WAIT during the floorless window — assembled from two
halves, one read and one reasoned.** Read: `EVTPASSES` moved once in 259 s and host CPU sat at
0.2–0.7 % user with 99.1–99.8 % idle (`docs/nsf-64-1-measurements.md` §4), and the only WAIT in
the loop is `nsfevt_plat_wait` → `@@ECBWL`. Reasoned: **no RB was read in that window.** The
`WCF=1` / `RBXWAIT` / `RBECBWT` signature that shows what a committed WAIT looks like on this
stand is 64-0c's REF1, taken on a **fresh** instance in a different round
(`docs/nsf-64-0c-measurements.md` §5). The two halves agree, and neither is the other.
