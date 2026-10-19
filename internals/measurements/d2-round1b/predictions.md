# d2 round 1b: the lower-case SSCT name, live predictions

Written 2026-10-14, **before any deploy**, and not edited afterwards. The
record goes in a separate file beside this one.

Build under test: branch `d2-round1` with the lower-case naming rule
(ADR-0048, annotation of 2026-10-14). mbt 3, cc370 1.6.3, libc370 2.11.0.
Stand: dev1.

## Q1. The two-name gate, now with lower-case names

`TSTSCTM PARM='GATE'`, batch and TSO:
- `nsz1` and `nsz2` register on the real chain, each found under its own
  name, neither under the other's.
- `NSZ1` (upper case) is **not** found while `nsz1` is chained.
- Removal is by name; nothing is left chained.
- The full API under the job's own derived name gives the key in lower case.

Round 1 had 24 assertions, and the new upper-case check adds one per step:
**25 PASS / 0 FAIL per step.**

**Falsified if** MVS refuses or mangles a lower-case name: register fails, or
`nsz1` is found as `NSZ1`. The naming rule would then be unusable, and that is
the outcome that would send the decision back to Mike.

## Q2. THE ROUND-1 FAILURE, REVERSED: `S NSFS` starts with `nsfs` chained

1. `TSTSCTH` (`PARM='HOLD'`) chains `nsfs` and ends: `TSTSCTM: HOLD nsfs
   CHAINED`.
2. `S NSFS` **starts** (`NSF040I`, `NSF001I`). Round 1 got `IEF612I PROCEDURE
   NOT FOUND`. NSFS says **`NSF059W RENDEZVOUS NOT REGISTERED ... ALREADY
   CHAINED?`**, because it refuses to chain a second one.
3. `P NSFS` takes the free path (no client in flight). `nsfsct_remove()`
   removes `nsfs` by name.
4. `TSTSCTP` reports **`PROBE nsfs ABSENT`**.

**Falsified if** step 2 gives `IEF612I`: lower case did not keep the name
apart from the procedure. Or if step 4 reports `CHAINED`: the free path did
not remove it, and the recovery aid `TSTSCTR` is needed.

## Q3. The ordinary start registers under the lower-case name

`S NSFS` on a clean chain says `NSF060I RENDEZVOUS REGISTERED -- ANCHOR=...`;
`TSTSCTP` then reports `PROBE nsfs CHAINED` with that anchor. After `P NSFS`
(free path) it reports `ABSENT`.

## Not run this round

The retain path. Round 1 measured it (NSF054W, then `CHAINED` after the stop),
and its code is unchanged. Re-inducing it costs 139264 bytes of CSA until IPL,
and Q2 produces the state that matters, a chained SSCT under the derived name
at `S NSFS`, without that cost.

Adoption is round 2.
