# M5-2e Job A -- the exit gate: TWO clients at once

**Date:** 2026-09-03 · **Stand:** MVSCE on mvsdev, NSFS STC01705/01706
**Predictions:** `predictions.md`, written before any deploy, amended once
before the run (the amendment is appended, the original kept verbatim).
**Proof kind:** the three properties are **LIVE**. The host suite figure is a
**no-regression check only** and evidence of nothing else -- the new test is
`host = false`, and the M5-2d1 ownership check it exercises takes the
zero-identity red line in Phase 1, so no host test can reach it.

**Result: both jobs CC 0000 -- A 18/18, B 23/23. Every prediction held.**

Job A is **pass/fail** and reports **no timing figures**. The per-interval
counts below are progress evidence for §1.1 and are **not comparable with
anything Job B produces**; Job B runs after this round and after an IPL.

---

## What was built

`test/mvs/tstrqx2.c` (`TSTRQX2`, `host = false`), `jcl/RQX2A.jcl`,
`jcl/RQX2B.jcl`, one `project.toml` block. **No module source:** `git diff
--name-only main` matches nothing under `src/`, `asm/`, `include/`,
`samples/`, so `asm/nsfvsvc.asm` is untouched, the anchor layout is unmoved,
`ANCVERNO` is 3, `NSFRQE` is frozen at 64 B and `rsvd`@60 is unspent.

It talks to the **private SVC directly** -- `NSFV_REQ.func = NSFV_REQ_RQE`,
`rqeimg` at a hand-built 64-byte `NSFRQE`, `ubuf`/`ulen` also in the
`NSFV_REQ` -- and links only itself plus `asm/nsftime.asm`, the TSTRQXC shape.
Two reasons, and the first is forced: §1.2 needs a client to **name a foreign
descriptor**, which the EZASOKET facade cannot do at all (it numbers sockets
in the client's own table). Given that, a gate whose subject is entirely
STC-side is better off with no copy of the stack linked into the client, so
"which copy ran" can never be a question. `nsfreqx_result_out` carries
`retcode`/`errno_`/`apptok`/`p1`/`p2`/`p3` back, so a raw client reads exactly
what the facade would have handed it -- checked in source before the shape was
chosen.

---

## Results against the predictions

| | prediction | result |
|---|---|---|
| **P0** | the deploy is seen to take effect | **held**, positively, both datasets |
| **P1** | §1.1 both clients progress in every interval | **held** |
| **P2** | §1.2 five rows, each negative with its control | **held**, all five |
| **P3** | §1.3 predicted to PASS | **held** -- `dirty=0`, 428 822 calls |
| **P3b** | the wire arm will NOT run, and will say so | **held** |
| **P4** | the round leaves the stand as it found it | **held** |

### P0 -- the deploy, and what the fingerprint actually is

This round changes **no module source**, so there is no behavioural
fingerprint separating "redeployed" from "not redeployed" and none is needed:
the two are the same content. (m5-79 measured the byte-level version -- two
builds of identical source differ in two bytes of build timestamp, so a module
hash is not usable as evidence.) What was established instead, positively, on
two separately-deployed datasets:

- **`NSF.LINKLIB` carries the M5-2d1 ownership check** -- §1.2's negative arm.
  A's target is left in `TCP_CLOSED`, so on an NSFS predating
  `nsfreq_sock_owned` B's `LISTEN` would have **succeeded**. It was refused.
- **TESTLIB is this round's** -- `TSTRQX2` ran, which is impossible on any
  earlier TESTLIB (`IEA703I 806-4` is the alternative, not a quiet wrong
  answer). JOB03038 shows it: `TSTRQX2: NO ROLE ('') -- nothing ran` ->
  `GATE SKIPPED -- CC 20, NOT a pass`, step CC **0020**. That single line is
  both the fingerprint and a live demonstration that a missing PARM cannot
  report a clean pass.
- Both deploys' output was read for the mid-chain `HTTP 500 ... Dataset
  delete failed`; neither appeared. The §5 tell (identical values across
  supposedly different builds) and its inverse (the mixed-build signature, a
  value neither pure build can produce) were both watched.

### P1 -- §1.1 multiplexing

Per-interval requests completed, **not a throughput figure**:

| t_ms | A | B |
|---:|---:|---:|
| 15000 | 38 438 | 38 416 |
| 30000 | 35 782 | 35 699 |
| 45000 | 36 037 | 35 865 |
| 60000 | 36 036 | 35 830 |
| 75000 | 33 030 | 33 160 |
| 90000 | 35 324 | 35 203 |

Neither client has a zero interval; the two track each other within 0.5 % in
every one. `bulk=214648` (A) and `214174` (B).

**The barrier skew is measured, not assumed.** Both jobs print opening and
closing TOD and both read one hardware clock, so they subtract exactly: A
opened **0.484 s** after B and closed 0.484 s after it, each window 90.000 s,
**overlap 89.516 s = 99.46 %**.

**And the contention is corroborated from the STC's own side, against a
negative control taken in the same STC instance minutes earlier.**
`F NSFS,STATS` after the round reads **`COLLISIONS=212463`** over 428 860
requests -- a claim scan repeatedly found a slot that was not FREE, about 0.5
times per request -- while `TSTRQXC` SOLO, run at 11.59.44 on this same fresh
instance, reported **`coll 0->0`**. One client moves the counter not at all;
two clients move it 212 463 times. That is real, slot-level interleaving
between two address spaces, and it is not something either client can see from
its own side. `EXHAUSTED=0`: with 64 slots and two clients nobody was ever
turned away, so the contention here is **service serialisation, not slot
starvation** -- two facts a single client cannot produce and that a
served/refused split alone would have conflated.

**The predicted serialisation is quoted as written.** Two clients are
multiplexed onto a **serial** service (ADR-0042 §10 permits exactly one
request in flight; concurrent service is a named open item, ADR-0043 gap (b)).
Nothing in this round changes the concurrency model, and no rate is reported.

### P2 -- §1.2 isolation, five rows and five controls

| row | observed |
|---|---|
| B `LISTEN` on **A's** `00010000` | `retcode=-1 errno=9` (**EBADF**) |
| A `LISTEN` on **that same** `00010000` | `retcode=0` (**RETOK**) |
| B `LISTEN` on **its own** `00010001`, first listen | `retcode=0` (**RETOK**) |
| A `LISTEN` on `00010000` **after A's TERMAPI** | `retcode=-1 errno=9` (**EBADF**) |
| B's sockets + app slot after A's TERMAPI | still work, **by use** |

**`EBADF` alone is not evidence and was never treated as one.** ADR-0046 makes
foreign and never-existing indistinguishable *by construction*, which is
exactly why a refusal has to be paired: **A was served on the same value**, so
the value B aimed at demonstrably resolved to a live socket.

**And the aim is a determination, not a derivation.** Both clients assert
their descriptors against **exact compile-time constants** -- A printed
`00010000 (the gate needs 00010000)`, B printed `00010001 (the gate needs
00010001); A's is therefore 00010000`. TSTD1B derives A's as B's index minus
one, which gets the *index* right and says nothing about the *generation* --
and a stale generation refuses through the same `NULL` as a foreign
descriptor. Asserting both whole descriptors puts the generation inside the
claim. If either constant had not matched, the run **skips (CC 20)** rather
than reporting a refusal it cannot attribute.

**B's survival arm is not vacuous, because A's own before/after row is its
control.** Without A's post-TERMAPI `EBADF`, "B survived A's TERMAPI" would be
green even if TERMAPI had torn nothing down. B's stream socket answers
**`errno=22` (EINVAL), not EBADF** -- the positive form: it resolved, it is
still B's, and it is still LISTENING. B's datagram socket is **driven**, not
merely resolved. B's app slot answers by granting a **new** socket on B's
token, which `do_socket` refuses for a token `app_index` cannot resolve.

`NSFSOC opens 5 closes 5` -- A's two, B's two, plus B's post-TERMAPI probe
socket -- so the **leak gate is clean** and the teardown counts reconcile.

### P3 -- §1.3, the landing area

`dirty=0` on both sides across **428 822** bulk calls, each verifying the
**whole** buffer after **every** call. `txerr=0`, `wrong=0`.

The mechanism the record should keep: **`g_land` is one buffer shared by
sequential clients**, safe because `g_busy` is held from dispatch to
completion, so a second request cannot enter it between one request's copy-in
and its copy-out. That was predicted to pass, in writing, before the run --
the point was to pin **positively** a property that rests on an invariant one
function away. A surprise here would have been a finding about ADR-0042 §10,
not about this test.

Three things make the green worth something, and none of them is the count:

- **Different generators** (`0xA1 + 7i + i>>5` against `0xB2 ^ (13i + i>>3)`),
  not one seed plus a constant offset, so a foreign byte is recognisable *as*
  the other client's rather than merely wrong -- and the mismatch report says
  which of the two it is.
- **Different lengths**: A stages 2048 (the whole landing area, maximum
  surface), B stages 1536, so B's `[1536, 2048)` is **never written by the
  transport at all** for the whole run -- a tail sentinel against an
  over-long copy-out that two equal-length clients could not see.
- A **pattern**, not zeros, so a byte that failed to cross shows as a mismatch
  instead of an accidental match.

### P3b -- the wire arm did not run, and said so

Both jobs: `wire=0 ok=0  *** NO INTERFACE -- WIRE ARM DID NOT RUN ***`, the
console `TSTRQX2: NO INTERFACE -- WIRE ARM DID NOT RUN`, and an assertion in
**positive form** (`n_wire == 0`, "the wire arm DID NOT RUN") rather than an
omitted assertion -- an omitted one prints nothing, which is the
absent-vs-succeeded shape this round is built to avoid.

**Corroborated from the STC side:** `LNK1 oerr 2` -- exactly two output
errors, one per client, which is the one `sendto` each made before giving up.

### P4 -- the stand

`NSF055I ... LARGEST FREE BLOCK NOW 1073152` before the round **and** on the
restart after it -- **no CSA debt**. Clean stop both times (`NSF830I` ->
`NSF043I SVC 239 RESTORED` -> `NSF044I` -> `NSF011I` -> `IEF404I`), **no
`NSF054W`**, so both clients' flag slots were lowered and nothing was left in
flight. **Zero dumps** (`IEA995I` = 0) -- and zero `IEF450I` abends too, so
that zero is "nothing failed", not "a dump was suppressed".

### The request count reconciles exactly

`NSFREQ recv` = `SERVED` = **428 860** = 428 822 bulk + 22 lifecycle verbs
(A 9, B 13) + **16** from `TSTRQXC` SOLO's two runs at 11.59.44 (8 requests
each, `XC_FN_UNKNOWN`), which is also exactly `NSFREQ badfn 16`. Every request
the STC served is accounted for.

---

## Findings

### 1. A source contradiction with the kickoff (source wins)

The kickoff's §2.2, verbatim:

> `sock_lookup` has **exactly one** call site, inside `nsfreq_sock_owned`.
> There is no bypass.

There are **two** in production code: `src/nsfreq.c:642` inside
`nsfreq_sock_owned` (`SOCK_LOOKUP: CHECKED`) and `src/nsfsoc.c:281` inside
`soc_complete` (`SOCK_LOOKUP: INTERNAL`). `tools/check-sock-lookup.sh`
independently prints `2 call site(s), all classified`.

**"There is no bypass" survives, but the reason given for it does not.** The
internal one is not a client-directed resolution: the stack is completing a
request it already holds and resolves that request's own socket to clear its
`pend_` slot, and all it can do is null a pointer equal to *this* request --
which only the socket that parked it can be. So the property holds because
that call site is **classified and harmless**, not because it does not exist.
`tstrqx2.c`'s header states the corrected enumeration (9 / 2 / 2).

**The mechanism of the error is duller than the finding and worth more:** the
grep was scoped to **one file** and the sentence was scoped to **the tree**.
The enumeration was offered as the proof and was itself unenumerated. That "no
bypass" survived is luck -- it survives because the second site happens to be
classified and harmless, and an enumeration that missed a site could as easily
have missed a live one.

> **An enumeration offered as proof must be scoped to what was actually
> enumerated.**

Recorded here with its siblings -- *a chain read out of source is a PREDICTION
until a run* (#101 Stage 1), *a falsification clause is a claim and needs the
same check as the prediction it guards* (#101 Stage 2), and *when a stimulus is
unconfirmed, check EVERY assertion that could depend on it* (the d1 SELECT
annotation). Promotion to CLAUDE.md §8.5 is Mike's convention call; nothing is
promoted here.

### 2. The gate could not run as designed, and the fix is a better gate

`tun0` does not exist on this stand: Hercules failed to create it at startup
(`HHC00138E Error setting TUN/TAP mode : Interrupted system call` ->
`HHC01463E 0:0501 device initialization failed`), so MVS IPLed without
0500/0501 -- the failure `docs/measurements/m5-79/` already records. NSFS
starts anyway and its transport is fully READY (`NSF212E CTCI 0500 FAILED TO
START`, then `NSF055I`, `NSF042I`, `NSF041I`, `NSF001I`), but `nsfip_route`
returns NULL, so **every `sendto` answers `EHOSTUNREACH` before `EMSGSIZE` is
ever reached**.

The kickoff locked §1.3's verb to `sendto` and, one paragraph later, wrote
"nothing here needs `tun0`". **Both cannot be true**, and the gate as first
written would have gone red for a reason with nothing to do with the landing
area -- the exact failure the kickoff's own red line names.

Mike's ruling: the **bulk** verb becomes a **non-blocking `RECVFROM` on an
empty rxq**, answered `EWOULDBLOCK` by `udp_recv` with no routing table, no
device and no wire, while the transport still stages `ulen` bytes in and
copies the same `ulen` back out. `SENDTO` stays as the once-per-checkpoint
wire arm, reported rather than asserted when there is no interface.

Two things this keeps and one it gains:

- The locked decision's **reason** is intact -- a **fixed** expected buffer
  range, which was the whole argument against a TCP send, whose short returns
  would make the expectation move from call to call.
- `EWOULDBLOCK` is a slightly **stronger** "really serviced" control than
  `EMSGSIZE`: it comes from a protocol op reached **through** the ownership
  check, so each of the 428 822 bulk calls also re-exercises §1.2 for that
  client's own socket.
- The gate is now **device-independent**, so it will not cost an IPL the next
  time `tun0` fails to come up -- which, on this stand, is a recurring event.

### 3. A latent coupling in the checkpoint block, found by review not by a test

The wire send was inside the `g_nchk < XA_CHK_MAX` guard, so a later change to
`XA_CHK_S` that overran the checkpoint array would have **silently stopped the
wire arm** while `n_wire_ok == n_wire` stayed green -- an assertion that stops
being exercised without saying so. The crossing now drives the wire send and
only the *recording* is capped.

---

## Round hygiene

- Offline gates: cross-build **6 modules + 57 test modules** clean; the new
  file built once with `-Wall -Wextra` temporarily added to `[build].cflags`
  and **verified to discriminate** (an injected unused variable produced
  `tstrqx2.c:989: warning: unused variable`; flags reverted, and the reverted
  state is in the committed `project.toml`). `tools/check-card-columns.sh` OK.
  `tools/check-sock-lookup.sh` OK. Alias scan: 269 aliases, all unique, all
  <= 8; `cc370 -S` shows `TSTRQX2` exports exactly **one** symbol (`MAIN`
  `ENTRY=YES`, 21 others `ENTRY=NO`), so no external symbol was added.
- Host suite **3491 PASS / 0 FAIL, 27 tests**, unchanged -- a **no-regression
  check only**.
- `make test-mvs` reported `Error 99` on the runner poll; the runner
  (JOB03038) **had already run** -- the documented "NO RC while the job
  actually finishes" friction. Confirmed by jobid, not assumed.
- The two jobs are hand-submitted concurrently, which the sequential runner
  cannot do. **Both CCs were read**, not only B's -- the `role_a` finding
  named a job whose result nobody looked at, and A carries three of §1.2's
  five rows.

## What this round does NOT establish

- **Throughput or latency of anything.** No figure here is a baseline, and
  none is comparable with Job B.
- **Concurrent service.** The model is unchanged and serial.
- **A protocol op READING the landing area.** The wire arm did not run. Every
  bulk call is answered before `udp_recv` touches `ubuf`, so what is proven is
  the transport's copy pair, not an op's use of it. Tracked as item 2 of
  **`docs/measurements/awaiting-ctci-pair.md`** -- one place, with d1's SELECT
  stimulus, rather than a footnote here that nobody returns to.
- **The partial-write residue shape** -- an op writing fewer bytes into
  `g_land` than `xlen`. The detector here is the identity round trip at full
  surface.
- **A bypass of the ownership check.** That is a source property (finding 1)
  and no runtime test can establish it; what the run adds is that the check
  holds live, across a real boundary, with two real clients.
- **d1's SELECT arms.** #107 assigns them to a d1 round. (e) does not cover
  them and must not be read as having.
- **Hardware arbitration of a simultaneous `CS`.** `collisions` cannot
  separate "the slot was already busy" from "lost a simultaneous race"
  (ADR-0042); the stand runs `NUMCPU 2` so it is physically possible rather
  than merely defended against, but possible is not observed.
