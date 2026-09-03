# M5-2e Job A -- predictions, written BEFORE any deploy or run

Written 2026-09-03 against `main` + the Job A branch, offline gates green
(cross-build 6 modules + 57 test modules, no warnings; card + sock-lookup
checkers OK; host 3491 PASS / 0 FAIL, unchanged, a **no-regression check
only**). **Not edited after the first live submit.**

Each prediction carries its falsification clause, and **the clause is checked
as carefully as the prediction** -- #101 Stage 2 recorded the reason: a
falsification clause is itself a claim, and an unexamined one turns a true
observation into a false conclusion.

---

## P0 -- the deploy is seen to take effect, and the fingerprint is stated

**This round changes NO module source.** `src/`, `asm/`, `include/` and
`samples/` are untouched, so `NSF.LINKLIB` cannot behave differently from
whatever it behaved like before. There is therefore **no behavioural
fingerprint that separates "redeployed" from "not redeployed"**, and none is
needed -- the two are the same content. (m5-79 measured the byte-level
version of this: two builds of byte-identical source differ in exactly two
bytes, a build timestamp, so a module hash is not usable as evidence either.)

What IS established positively, and each is a different question:

1. **That NSFS carries the M5-2d1 ownership check** -- §1.2's negative arm.
   A's target socket is left in `TCP_CLOSED`, so on an NSFS predating
   `nsfreq_sock_owned` B's `LISTEN` on A's descriptor would **succeed**. The
   assertion cannot pass on that module.
2. **That TESTLIB is this round's** -- `TSTRQX2` running at all. It does not
   exist on any earlier TESTLIB, so the alternative is `IEA703I 806-4`, not a
   quiet wrong answer. Note the two datasets are separately deployed:
   `NSF.LINKLIB` by `make deploy`, TESTLIB by `make test-mvs`.
3. **The §5 tell** (identical values across supposedly different builds) and
   **its inverse** (a value neither pure build can produce -- the mixed-build
   signature) are both watched at every deploy, and every deploy's output is
   read for the mid-chain `HTTP 500 ... Dataset delete failed`.

**Falsified if** B's foreign `LISTEN` is served, or `TSTRQX2` draws `806-4`.
*Clause checked:* both are positive observations, not absences -- neither can
be satisfied by something failing to happen.

---

## P1 -- §1.1 multiplexing: both clients progress in EVERY interval

Both jobs report **more than one checkpoint** and a **non-zero completed count
in every interval after the first**. The two clients' per-interval counts will
be broadly similar, because the claim scan is not a queue and makes no fairness
promise but neither client holds a slot between its own requests.

**This is not a throughput prediction and no rate is reported.** Job A is
pass/fail. The per-interval counts are progress evidence and are **not
comparable with anything Job B produces** -- Job B runs after this round and
after an IPL.

**Predicted serialisation, quoted as written whatever the run shows:** two
clients are multiplexed onto a **serial** service (ADR-0042 §10 permits exactly
one request in flight; concurrent service is a named open item, ADR-0043 gap
(b)). If the combined progress looks disappointing, that is the specification
and not a defect, and nothing in this round changes the concurrency model.

**Falsified if** either client shows a zero-progress interval while the other
progresses. *Clause checked:* the two ways this could be a false alarm are
(a) the sampling never fired -- guarded by its own `g_nchk > 1` assertion, so
a table of zeros from an unsampled window cannot be read as starvation; and
(b) a client legitimately finished its window early -- impossible, the window
is closed by the same clock that stamps the checkpoints and the loop breaks
only past `XA_WIN_S`.

---

## P2 -- §1.2 isolation: refused foreign, served own, and TERMAPI really tears down

Five rows, and each negative has its control **in the same run**:

| row | predicted |
|---|---|
| B `LISTEN` on **A's** descriptor | `RETERR` + `EBADF` |
| A `LISTEN` on **that same** descriptor | `RETOK` -- the control |
| B `LISTEN` on **its own** (first listen) | `RETOK` -- the control |
| A `LISTEN` on its own **after A's TERMAPI** | `RETERR` + `EBADF` |
| B's socket + app slot after A's TERMAPI | still work, **by use** |

**Falsified if** B is served on A's descriptor (the check is absent or
address-space scoping is wrong), or if A is refused on its own (the check is
too strict -- which would refuse every honest client and is the failure mode
d1's round §2.1 was built to catch), or if B's sockets die with A's.

*Clause checked, and this is where a vacuity would hide:* `EBADF` alone is
**not** evidence, because ADR-0046 makes foreign and never-existing
indistinguishable by construction. What makes the refusal a refusal is A being
**served on the same value**, and what makes the value the right one is that
both clients assert their descriptors against **exact compile-time constants**
rather than deriving them -- a derivation gets the index right and says nothing
about the generation, and a stale generation refuses through the same `NULL`.
If either constant does not match, the run **skips (CC 20)** rather than
reporting a refusal it cannot attribute.

*Second clause checked:* a red row here would first be read as a **stand**
problem (a stale socket table), and the SKIP path exists precisely so that
case cannot present as a failure.

---

## P3 -- §1.3 the landing area: PREDICTED TO PASS

Both clients' buffers hold their **own full pattern after every call**;
`dirty = 0` on both sides.

**Why, stated before the run:** `g_land` is one buffer shared by sequential
clients, and it is safe because `g_busy` is held from dispatch to completion,
so a second request cannot enter it between one request's copy-in
(`src/nsfsx.c` copy IN) and its copy-out. Proving it **positively** is the
point: it pins a property that today rests on an invariant one function away,
and it has never been exercised with two real clients alternating through it.

**A surprise here would be a finding about ADR-0042 §10, not about this test.**

**Falsified if** either client's buffer differs from its expected pattern.
*Clause checked -- and this is the clause that matters most, because a red
§1.3 must NOT be read as "concurrent service landed".* There are at least four
non-concurrency ways to fail it, and the test distinguishes the first from the
rest:

1. **contamination** -- the mismatching byte equals the OTHER client's
   generator at that index. The two clients use genuinely different generators
   (`0xA1 + 7i + i>>5` against `0xB2 ^ (13i + i>>3)`), not one seed plus a
   constant offset, exactly so this is decidable. The report says which.
2. **wrong `xlen`** -- a short or long staged count. Shows as a mismatch that
   is *neither* pattern, and the report says so.
3. **an over-long copy-out** -- B's `[1536, 2048)` is never written by the
   transport at all, so a copy-out past the staged length lands there. This is
   why the two clients send **different lengths**; two equal-length clients
   could not see it.
4. **a bug in this test** -- the fill, the expected buffer, or the re-arm
   `memcpy` that stops one contamination event cascading into thousands.

Only (1) is a statement about concurrency.

---

## P4 -- the round leaves the stand as it found it

`LARGEST FREE BLOCK` unchanged across the round (today's clean-start
expectation is `NSF055I ... 1073152`), **no `NSF054W`**, zero dumps, both
clients' flag slots lowered, pool 64/64 FREE.

**Falsified if** an `NSF054W` appears -- and per the kickoff that **discards
the run** rather than footnoting it: an IPL is available on demand, so a
discarded run is a repeat, not a blocked round.

---

## What Job A does NOT test, said before the run rather than after

- **Throughput or latency of anything.** No figure here is a baseline.
- **Concurrent service.** The concurrency model is unchanged and serial.
- **The partial-write residue shape** -- an op that writes FEWER bytes into
  `g_land` than `xlen`, leaving the tail as the previous content. Every bulk
  call here is refused `EMSGSIZE` before the op reads or writes the landing
  area at all, and the in-range call is the **only** case in the whole file
  where the op actually READS `g_land`. The detector is the identity round
  trip at full surface, not the residue shape.
- **A bypass of the ownership check.** That is a source property (the
  enumeration in `test/mvs/tstrqx2.c`'s header) and no runtime test can
  establish it. What the run adds is that the check holds live, across a real
  boundary, with two real clients.
- **d1's SELECT arms.** #107 assigns them to a d1 round; (e) does not cover
  them and must not be read as having.

---

## AMENDMENT, before any run -- the bulk verb changed on a STAND finding

**Appended, nothing above rewritten** (the #101 Stage 2 form: an amendment
made before the run, with the original kept verbatim so the change is
visible rather than invisible).

**What was found.** `tun0` does not exist on the stand: Hercules failed to
create it at startup (`HHC00138E Error setting TUN/TAP mode : Interrupted
system call` -> `HHC01463E 0:0501 device initialization failed`), so MVS
IPLed without 0500/0501 -- the failure `docs/measurements/m5-79/` already
records. NSFS still starts and its transport is READY (`NSF212E CTCI 0500
FAILED TO START`, then `NSF055I ... 1073152`, `NSF042I`, `NSF041I`,
`NSF001I`), but `nsfip_route` returns NULL, so **every `sendto` answers
`EHOSTUNREACH` before `EMSGSIZE` is ever reached**.

**Why that is a design problem and not just bad luck.** P3 above would have
gone red for a reason with nothing to do with the landing area -- the exact
failure the kickoff's own red line names ("a gate should not go red for
reasons unrelated to the property it tests"), and its statement that "nothing
here needs `tun0`" was false of the gate as first written.

**The change (Mike's ruling).** The BULK verb becomes a **non-blocking
`RECVFROM` on an empty rxq**, answered `EWOULDBLOCK` by `udp_recv` with no
routing table, no device and no wire, while the transport still stages `ulen`
bytes in and copies the same `ulen` back out. `SENDTO` remains as the
once-per-checkpoint **wire arm**.

**P3 is unchanged in substance and its falsification clause still holds** --
the four failure modes enumerated there are properties of the transport's
copy pair and of this test, not of the verb that rides it. Two riders:

- The expected buffer range is still FIXED, which was the locked decision's
  whole reason for preferring `SENDTO` to a TCP send.
- `EWOULDBLOCK` is a slightly STRONGER "really serviced" control than
  `EMSGSIZE`: it is returned by a protocol op reached THROUGH the ownership
  check, so every bulk call also re-exercises §1.2 for this client's own
  socket.

**New prediction P3b -- the wire arm will NOT run this round**, and will say
so: `wire=0 ... *** NO INTERFACE -- WIRE ARM DID NOT RUN ***` in both jobs'
summaries plus `TSTRQX2: NO INTERFACE -- WIRE ARM DID NOT RUN` on the
console, with an assertion that states the arm did not run rather than a
silent absence.

**Falsified if** the wire arm runs (the interface came up after all -- which
would be good news and is checked from `NSF210I`/`NSF211I` before the arms,
not inferred from the test), or if it neither runs nor says so.

*Clause checked:* the dangerous reading here is a green run being taken for a
run that included the wire arm. That is why the skip is asserted in POSITIVE
form (`n_wire == 0`, "the wire arm DID NOT RUN") rather than by omitting the
assertion -- an omitted assertion prints nothing at all, which is the
absent-vs-succeeded shape this round is built to avoid.

**CONSEQUENCE FOR THE RECORD, stated now rather than after:** this round will
NOT establish anything about a protocol op *reading* `g_land`. The three
properties are unaffected -- they are all answered inside the STC -- but the
"does NOT test" list above gains this row explicitly.
