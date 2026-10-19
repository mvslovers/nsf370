# d2 round 1 — predictions, written BEFORE any deploy or live run

Written 2026-09-17 against `main` + the round-1 branch, offline gates green:
host **3547 PASS / 0 FAIL** (TSTSCT 56, and **verified to discriminate** — see
P0b), cross-build **6 modules + 58 test modules** clean, alias scan **256 unique
≤ 8**, card check and sock-lookup check OK. **Not edited after the first live
submit.**

**Round 1 only: register, find, remove.** No adoption. Every clause below that
sounds like adoption is a scope statement, not a plan.

---

## P0 — the deploy is seen to take effect, and the fingerprint is stated

**This round DOES add a message path**, which the previous two could not use:
`NSF060I RENDEZVOUS REGISTERED -- ANCHOR=…` on a successful `S NSFS`, or
`NSF059W RENDEZVOUS NOT REGISTERED …` on a refusal. **Neither string exists in
any previous build**, so its presence is a positive fingerprint for
`NSF.LINKLIB` — the thing the Job A wire round and the d1 stimulus round both
had to work around.

For **TESTLIB** the fingerprint is `TSTSCTM` running at all: it exists on no
earlier TESTLIB, so the alternative is `IEA703I 806-4`, not a quiet wrong
answer.

**Both §5 tells are checked and both are inapplicable in the usual way** — one
build, so *identical values across supposedly different builds* has nothing to
compare, and *a value neither pure build can produce* needs two pure builds.
`NSF060I` replaces them.

**Falsified if** `S NSFS` produces neither `NSF060I` nor `NSF059W`, or `TSTSCTM`
draws `806-4`.

## P1 — the two-name gate passes, and it is two names and not two stacks

`TSTSCTM` (no PARM) registers **`NSZ1`** and **`NSZ2`** on the real SSI chain,
finds each under its own name, shows neither finds the other's, removes each by
name, and asserts **nothing is left chained**.

**What it establishes:** the registration is keyed by a NAME, not a place —
which is exactly what the fixed CSA cell and the stolen SVC slot failed.
**What it does NOT establish, and the record says so in the same breath: that
two stacks can run.** Only one instance can steal SVC 239, so a second NSFS is
not available; the rendezvous simply does not need one to be tested.

**Falsified** by any red assertion, or by the SKIP path firing — which means a
previous run leaked a throwaway name and this one could not be aimed.

*Clause checked.* The way a green P1 could be hollow is a run that registered
nothing and therefore refused nothing. Closed by the controls: every "not found"
is paired with the same name **being found** earlier in the same run, and the
final assertion is that the chain is clean — which cannot pass if registration
never happened, because the removals would have failed first.

## P2 — the retain/free asymmetry, and this is the dangerous line

**`nsfsx_stop` removes the SSCT on the FREE path only.** The retain branch must
leave it chained: the anchor is being retained precisely because a client is
parked in it, and a later start can only find it if the registration outlives
this STC.

**How the gate distinguishes the two paths** — a test program cannot, because
both need a real `P NSFS` either side, so it is operator-driven and read from
**two `TSTSCTM PARM='PROBE'` runs**:

| stop path | induced by | PROBE after the stop predicts |
|---|---|---|
| **free** (drained) | an ordinary `P NSFS` | `ABSENT` — the registration was removed with the storage |
| **retain** (`NSF054W`) | a client that leaks `inflight` (`TSTRQXC PARM='LEAK'`, b4's induction) | **`CHAINED`** — and the anchor address it reports is the retained one |

**Falsified** if the free path leaves it chained (a leak with nothing naming it
— the status quo ante, silently), or if the **retain path removes it**, which is
the failure this round exists to prevent and would look entirely correct in
every message on that path.

*Clause checked.* The reading that would be wrong here is a PROBE saying
`ABSENT` after a retain because registration never happened at all. Separated by
P0: `NSF060I` on the start says the registration existed before the stop.

## P3 — the stand

`LARGEST FREE BLOCK` at entry and exit. **This is a GATE, not a measurement**,
so the marker does **anchor detection only** (#119's correction); the entry
reading has moved three times on this stand (**#120**) and no absolute value is
treated as expected. **An `NSF054W` outside the deliberate P2 induction discards
the run.** Zero dumps, greps paired against known-present lines.

**One cost this round can incur and must not leave behind:** an SSCT under
`NSZ1`, `NSZ2`, or the test job's derived name. Any of those is registered until
IPL and would block a start under that name — the very failure round 1 prevents.
`TSTSCTM` asserts its own cleanup; the round also PROBEs afterwards.

## What this round does NOT establish

- **Adoption.** What a start does with a retained anchor. Round 2.
- **Two stacks running.** See P1.
- **The collision rule in production.** The jobname tail is carried and compared,
  and `FOREIGN` is returned — but no start acts on it, because acting is
  adoption.
- **That the real anchor offsets are right on the host.** The host shim shares
  field NAMES, not layout; the offsets are the cross build's and the live gate's.
- **A milestone flip.** Nothing flips to proven.
