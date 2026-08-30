# ADR-0045 — The app-registry sweep ships as best-effort, and is named that way

**Status:** Proposed (2026-08-30). Settles **what M5-2c1 delivers against obligation #3** —
reclaim the sockets of an application that ends without `TERMAPI` — now that three rounds of
measurement have established the obligation cannot be kept.

**It ships a feature that cannot keep its obligation's promise, and the naming is the
decision.** That sentence is the point of this ADR, not a caveat appended to it. The sweep
reclaims a narrow class soundly and everything else not at all; presenting it as a replacement
for `TERMAPI` would be false in a way that costs someone a debugging round, because it would
license writing a client that never calls `TERMAPI`. **`TERMAPI` remains the
contract**, and **issue #88 stays OPEN with its design deferred rather than closed.**

**Relates to:** ADR-0040 (the client-death guard — the *transport's* per-request pre-POST
check, which shares this sweep's classifier and nothing else; see §5), ADR-0041 (the NSFRQE
crossing that carries the caller identity), ADR-0042 §10 (one unit of work per pass — why the
sweep stands down while a request is in service), ADR-0043 (the cross-AS wake contract — why
the rate limit is not a timer), ADR-0034 (queue empty ⟺ STIMER disarmed — why it is not
ticks), spec §5.3, §10.5.

**Evidence pins:** `docs/measurements/40-chk/` (a batch client's guard verdict is `LIVE` and
the POST proceeds), `docs/measurements/40-ident/` (an STC identity dies within a second and is
then *resurrected* by ASID reuse; `(ASCB, ASID)` is an address, not an identity),
`docs/measurements/m5-2c1/` (stage a: the recorded ASCB is the **initiator's**, and three
different jobs reported the same one). Issue #88 carries the position and the study order.
Source pins: `src/nsfreq.c` (`nsfreq_app_sweep`, `do_initapi`), `src/nsfsx.c`
(`nsfsx_drain` step 0b, `NSFSX_SWEEP_SECS`, `NSF817I`), `src/nsftime_plat.c` +
`src/nsftime_plat_host.c` (the interval seam), `include/nsfreq.h`, `include/nsftime.h`.

---

## 1. Context — what was measured, and what it rules out

Obligation #3 asks for reclamation of an application's sockets when the application is gone.
Reclaiming means classifying its owner **DEAD**, and ADR-0040 never reaps an `UNKNOWN`. So the
whole feature rests on one question — can we tell that a client has ended? — and three rounds
answered it:

- **A batch client's recorded identity is the INITIATOR's, and an initiator does not end when
  a job ends.** A normal end and a `CANCEL` both read `LIVE`, at every one of 28 slot readings
  in stage a, and three different jobs reported the same `ASCB=00FE7B58 ASID=0006`. **The
  sweep reclaims nothing for batch. Ever.** This is not a defect in the classifier: it answers
  "did that address space end?" correctly, and for a batch client the answer is genuinely no.
  What died is the *task* inside a live address space, and nothing in this system asks that.

- **An STC client does die — within one second — but the verdict is transient.** 40-IDENT's
  two STC runs took the same ASCB *and* the same ASID, and starting a third flipped two
  provably dead clients back to `LIVE`: **reclassified, not reaped.** A `DEAD` verdict
  therefore states what occupies that ASID *right now*, not what happened to the recorded
  client.

- **There is no finer-grained alternative at this level.** The jobname repeats
  byte-identically for the same JCL; the job number needs private aliased storage the STC
  cannot reach; there is no `ASSB` and no `STOKEN` on 3.8j.

What survives is narrow and it is exactly what this ADR builds: **reclaim an app slot whose
ASID is not in the ASVT at the moment of the check.**

## 2. Decision

**Ship it, best-effort, and lead with what it cannot do everywhere it is described** — the
code comment, the operator message, this ADR and `CLAUDE.md`. None of them presents it as a
replacement for `TERMAPI`.

Concretely:

1. A sweep over the app registry classifies each in-use slot and, on `DEAD` only, runs the
   existing teardown checklist — `soc_foreach(term_one, …)` then `app_free`. That machinery is
   `do_termapi`'s, unchanged: a client that died **is** an application that never got to call
   `TERMAPI`, and it must die through the same one checklist (spec §10.5).
2. **Two triggers, one implementation, the cap as a parameter.** The Phase-2 executive drain
   passes a minimum interval; `do_initapi` passes `0` when the app table is full, which
   bypasses the limiter without being a special case in the code — a zero interval has always
   elapsed. Two paths that can drift is what `nsfsx_stop` cost this milestone once already.
3. `UNKNOWN` is never reclaimed, and neither is a slot with no identity recorded.

## 3. Why it is defensible to ship at all — the safe-side asymmetry

The two failure directions are not symmetric, and the sound one is the one this can produce:

- **A false `LIVE` leaks a slot.** Cost: one of 64 registry entries and its sockets, until the
  STC is recycled. This is the batch case, permanently, and the STC case after ASID reuse.
- **A false `DEAD` tears down a healthy application's sockets.** This one **cannot arise from
  reuse**, because an ASID is unique among the address spaces that are alive: reuse can only
  convert `DEAD → LIVE`, never the reverse.

So the sweep's entire error budget is spent on *not* reclaiming. That is what makes shipping a
feature with these limits defensible, and it is the same asymmetry ADR-0040 is built on.

**The consequence for the live gate follows from this and is not a matter of taste**: a run in
which nothing is reclaimed is **not automatically a failure** — it may mean the ASID was reused
before the check. The gate must therefore separate *"the sweep saw `DEAD` and did not act"* (a
defect) from *"the sweep saw `LIVE` because the ASID had been re-taken"* (expected). `NSF057I`
exists for that: it is emitted at the moment of a reclaim, which is the only point at which the
evidence still exists.

**That message alone is not enough, and the gap is the expected case.** `NSF057I` proves "saw
`DEAD` and acted". It cannot separate the other two, because both are *silent*: **swept, and
everything read `LIVE`** (reuse, or a batch client — expected) from **never swept at all** (no
executive pass, a request in service, or the interval not elapsed) — and "never swept" is the
likely default, since 64-1 measured one pass in 259 s on an idle executive. `F NSFS,APPS`
does not close it either: `DEAD` + the slot still in use + no `NSF057I` reads identically for
a defect and for a pass that never happened. So the sweep keeps two counters, reported through
the STATS **supplement** as `NSF817I APPSWEEP SWEEPS=n RECLAIMED=m`, and a live arm reads
`SWEEPS=12 RECLAIMED=0` against `SWEEPS=0` without anyone having to reason about where in a
pass the operator drain runs relative to the sweep.

**`SWEEPS` counts BOTH triggers, so it is a ceiling unless the arm controls for the other
one.** `do_initapi`'s on-demand sweep increments it too, so `SWEEPS=12` does not by itself
establish that twelve *periodic* sweeps ran — `INITAPI` churn would inflate it. Arm 1 must
therefore read it with no `INITAPI` traffic in flight, or read it as an upper bound.

They are **not** `sts_register` counters, and that is a measured constraint rather than a
preference: the NSFS build already registers ~46 counters and `sts_render` fills a fixed
512-byte buffer, so the rendered block truncates well before the end of the list — a counter
added there could be one that never reaches the console, which is evidence that silently does
not exist. The supplement is emitted *after* that block for exactly this reason.

## 4. Why a real-time interval, and not ticks or a timer

The limiter bounds how often the scan runs — 64 slots, each an ASVT lookup, on an executive
that makes thousands of passes a second when busy.

- **Not ticks.** ADR-0034 fixed the arming contract as *queue empty ⟺ STIMER disarmed*, so
  with nothing armed no NSFTMR-derived tick advances at all (64-1 measured one pass in 259 s).
  A tick-based limiter would never see its interval elapse **after an idle period**, which is
  precisely the pass that matters.
- **Not a timer.** Arming one keeps the STIMER permanently armed and reintroduces the idle
  floor ADR-0043 established is not required.
- **So: real seconds, behind a seam.** `include/nsftime.h` forbids deriving wall-clock from
  `nsf_now`'s `hi` word and forbids assuming a shared unit across platforms; that MVS happens
  to give 1.048576 s per `hi` unit and the host exactly 1 s is a coincidence, not a contract.
  `nsf_elapsed_ge` is therefore a platform pair (`src/nsftime_plat.c` /
  `src/nsftime_plat_host.c`), **C on both sides** — it is arithmetic over a value the seam has
  already produced, and hand-writing it in HLASM would buy nothing and cost the whole
  column-72 / `as370 -a=` gate.

It is **pure** — both timestamps are parameters — so the boundary is host-testable exactly;
a version reading the clock itself could not be pinned, because the reading would move between
the test building `since` and the function taking `now`. It **rounds late, never early**: a
rate limiter that fires slightly late is a rate limiter, one that fires early is not.

**What the ten seconds does NOT promise.** It bounds the interval *between sweeps*. It does
not promise that a dead client is reclaimed within ten seconds, because a sweep still only
happens on an executive pass and a pass only happens when something uses the stack. The honest
form is: **no sooner than ten seconds after the last sweep, and not until the stack is used
again.** The number is not load-bearing — reclamation that is late by seconds costs nothing,
since the slot was already leaked and nobody is waiting on it — and the moment it actually
matters, the table being full, `do_initapi` bypasses it entirely.

## 5. Two reclamation paths, and they are not the same path

ADR-0040's guard reaps **CSA request slots at the transport**, immediately before a reply POST.
This sweep reclaims **app slots and sockets in the executive**, periodically and on demand.
They share `nsfreqx_classify` **and nothing else** — different subjects, different storage,
different triggers, different failure modes. `CLAUDE.md` records that they must not be
conflated in code or in the record, which is also why this is a new ADR rather than an
annotation on 0040.

## 6. Placement, and two things that had to be decided rather than fall out

**The periodic sweep is in `nsfsx_drain`, not `nsfreq_drain`.** `evt_set_request` wires exactly
one drain per build — `nsfreq_drain` in Phase 1 (`src/nsfmain.c`), `nsfsx_drain` in Phase 2
(`src/nsfsmain.c`) — so a sweep in the Phase-1 drain would never run in Phase 2 at all. Putting
it there is also what makes **"Phase 1 sweeps nothing" structural** rather than a property of
the zero identity: `src/nsfmain.c` does not reach that code, registers no classifier and
registers no notify.

**It goes AFTER step 0, never in front of it.** The wake-ECB reset is load-bearing and its
argument is positional — 64-1's no-lost-wake reasoning depends on the reset preceding both
scans. Inserting a block ahead of a first-position invariant is the shape of the
`asm/nsfvsvc.asm` fall-through bug, in C.

**It stands down while a request is in service (`g_busy`), and that is a decision.** The
in-service request may be *parked on a socket owned by one of these very apps*; reclaiming
would run `soc_destroy` on that socket and complete `g_priv` from inside a scan, underneath the
step-1 completion path that owns the `g_busy` / `g_busy_slot` bookkeeping. This is the same
shape M5-2b4 found when a slot scan could have reaped the in-service CSA slot from under the
executive. The sweep is opportunistic; deferring it one pass costs nothing, and its interval is
measured in seconds while a pass is measured in milliseconds.

**No key window.** Unlike the scans around it, the sweep touches no CSA: the app registry is
STC-private key-8 storage, and the classifier only *reads* the CVT and ASVT, which are common
storage and not fetch-protected. Stage a ran exactly this path live from the `F NSFS,APPS`
operator verb, in problem state key 8.

## 7. The notification is a seam, not an `nsfmsg` call

`nsfreq_set_sweep_notify` exists for two reasons. Most builds that link `src/nsfreq.c` do not
link NSFMSG at all, so a direct WTO would force ~20 source-list changes and drag the operator
console into a portable file. And a WTO is **invisible to a host test**, whereas the reclaim —
including the identity the slot was carrying — is exactly what a test must be able to observe.
Phase 2 registers a wrapper that emits `NSF057I`, and a per-sweep summary `NSF058I` carrying
the count **and the caveat**; `NULL` is the default and the Phase-1 state.

**The numbers are 0xx, not 8xx, and that is the point of them.** These are *executive* actions
— the drain reclaiming storage — so they sit with their siblings `NSF050I` / `NSF051W`, which
the same file emits when the transport reaps a CSA request slot. The app-registry **operator
report** is 814–816 and the sweep's STATS supplement is `NSF817I`, because those are things an
operator asked for. §5 insists the two reclamation paths are not conflated; numbering the sweep
with the operator verb that merely reports it would conflate them where a reader looks first.

**`NSF058I` carries the caveat because "RECLAIMED" reads as authoritative cleanup to an
operator**, and the honest reading is that most dead clients are not reclaimed at all. It is
emitted once per sweep that reclaimed something — never per slot (64 lines in the mass-reclaim
case) and never on an empty sweep (a line every ten seconds, forever).

**It is emitted from INSIDE the sweep, through a seam of its own, and that is a correction.**
Written first as the periodic caller's own line, it silently exempted the other trigger — and
that trigger is the **full table at `INITAPI`**, the loudest and most consequential burst
there is, the very one §9 tells the reader to picture, and precisely where an operator most
needs to be told that most dead clients are never reclaimed. A summary that covers the quiet
path and not the loud one is worse than none, because it reads as complete. Putting it inside
`nsfreq_app_sweep` makes "every sweep that reclaims, summarises" structural, on the same rule
the sweep itself is built on: one function, two callers, no drift. Its width was
**measured, not estimated**: the Hercules console truncates around 107 characters and eats the
**tail**, and the natural phrasing came to 127 — the console would have kept the reassuring
half and dropped every word that qualifies it. So the caveat leads and the count sits inside
it, at 103.

The identity is captured **before** `app_free`, along with the token: `app_free` bumps the
generation *and* zeroes the identity, so reading either afterwards reports a slot that names
nobody — a notification of `ASCB=0 ASID=0`, which is precisely the identity a live run needs.

## 8. Mike's position, recorded as his and not as consensus

An explicit `TERMAPI` contract is **too fragile**. It moves the robustness onto the client, and
the client is precisely the component whose misbehaviour creates the problem. A TCP/IP stack is
essential infrastructure and must protect itself **implicitly**.

The best-effort sweep is an **interim measure, not the answer**, and the interim is accepted
only because the stack has to be seen working first. The real design is deferred to issue #88,
which carries the study order: the ancestor `mvs38j-ip` first; then mechanisms MVS releases
automatically at task or address-space termination (a client-side ENQ taken by the EZASOKET
stub at `INITAPI` is the named candidate — NSF's own code, linked into the application, so
relink-only still holds); then `TERMAPI` from an ESTAE exit for ecosystem clients.

## 9. Consequences

- An application that ends without `TERMAPI` leaks its slot and sockets **unless** it ran as an
  address space of its own *and* the sweep wins the race against ASID reuse. Batch: always
  leaked.
- `NSFREQ_APP_MAX` at 64 **buys time and fixes nothing**; raising a bound does not reclaim a
  slot. Said at the constant, pointing at #88.
- The rate limiter's state is one `NSFTIME` in `src/nsfreq.c`, reset by `nsfreq_init`, so a
  fresh stack sweeps on its first pass ({0,0} is deliberately "long ago" on both platforms).
- **A mass reclaim is a burst inside one run-to-completion pass.** The realistic path is a
  full table at `INITAPI`: up to 64 × (`soc_foreach` over the socket table + a `NSF057I` WTO),
  plus one `NSF058I`. Bounded, and it happens once — the slots are free afterwards — but it is
  the pass a reader should picture, not the steady-state one.
- **"Phase 1 is untouched" is a behavioural claim, not a byte-level one.** The `NSF` module
  links the changed `src/nsfreq.c` and the new `src/nsftime_plat.c`, and `do_initapi`'s
  full-table path now runs a 64-slot scan there before returning `EMFILE`. It reclaims
  nothing, because no classifier is registered and every Phase-1 slot has a zero identity, and
  Phase 1 registers no notify — so the observable behaviour is unchanged and `S NSF` need not
  be redeployed (the M3-2 precedent). The module is not byte-for-byte identical, and saying so
  plainly is cheaper than a reader discovering it.
- **Not established by this step:** anything about issue #88's eventual design; the TSO client
  class, still unmeasured; and the live behaviour of the periodic trigger under a real ASID
  reuse race, which the gate measures but cannot bound.
