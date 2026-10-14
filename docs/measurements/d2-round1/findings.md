# d2 round 1 — live results, and the finding that stops it

**Date:** 2026-09-17 · **Stand:** MVSCE-DEV on mvsdev (PID 1098) — **five
emulators now running**, not one (`MVSCE-DEV`, `-LAB`, `-EXP`, `MVSTK5-BLD`,
`-REF`); task set JES2/NET/TSO/HTTPD/UFSD/FTPD/NSFS. A pass/fail gate tolerates
the extra emulators; a measurement would not.

**Predictions:** `predictions.md`, written before any deploy, **not edited**.

---

## The three predictions HELD

**P0 — the deploy is seen to take effect.** `NSF060I RENDEZVOUS REGISTERED --
ANCHOR=00A8B7C8` on `S NSFS` — a string **no previous build contains**, and the
address matches `NSF041I`'s. TESTLIB by `TSTSCTM` running at all.

**P1 — the two-name gate.** `TSTSCTM` **24/24, batch and TSO** (`JOB00388`), on
the real SSI chain: `NSZ1` and `NSZ2` registered, each found under its own name,
neither finding the other's, `NSZ1` removed while `NSZ2` was left alone, removal
surviving a corrupt anchor, and **nothing left chained**. The full API ran under
this job's own derived name — `MBTTEST` → key `MBTT`, tail `EST ` — including
the refusal of a second register.

**P2 — the retain/free asymmetry, both directions, same build, same run.**

| stop | console | PROBE after |
|---|---|---|
| clean (free path), 7.57.10 | no `NSF054W` | **`ABSENT`** |
| retain, 7.58.38 | **`NSF054W 1 CLIENT(S) STILL IN FLIGHT`** | **`CHAINED 00A8B7C8`** |

**P3 — the stand.** Entry `1069056`; exit `929792`, exactly **−139264** = the
retained anchor + router from the deliberate induction (b4's precedent).
**Zero dumps** — `IEA995I` 0, `IEF450I` 0 — against positive controls of 13
`IEF404I`, 3 `NSF060I`, 3 `NSF055I` found by the same grep.

---

## AND THEN THE FINDING, WHICH INVALIDATES THE NAME

**`S NSFS` fails `IEF612I PROCEDURE NOT FOUND` while an SSCT named `NSFS` is
chained.** Measured at 7.59.42, immediately after the retain-path stop:

```
0000  7.59.42 STC  177  S NSFS
0000  7.59.42           IEF196I     2     IEF612I PROCEDURE NOT FOUND
4040  7.59.42           IEF452I NSFS     JOB NOT RUN - JCL ERROR
0000  7.59.42           IEE122I START COMMAND JCL ERROR
```

**The first start works and every later one does not.** Registration happens
*during* start, after the procedure name has already been resolved; a subsequent
`S NSFS` meets the chained subsystem name first and never reaches a procedure.

**So after ANY retain-path stop, the STC cannot be started again.** The
mitigation round 1 was built around — `nsfsct_register` refusing rather than
chaining a second SSCT — **never executes**, because NSFS's code never runs.
The guard is in the wrong place entirely: it is inside a program that the
condition prevents from starting.

**This was in my own document, quoted verbatim, and I did not carry it into the
naming decision.** ADR-0048 §2 quotes ADR-0036:

> While the SSCT is registered, `S NSFP` fails `IEF612I`; an abend that skips
> cleanup leaves the SSCT registered **until IPL**.

I read that as *"the next start either fails or chains a second one"* and built
against the second half. The first half is the one that bites, and it bites
before any NSF code runs.

**Consequence for the open naming question (ADR-0048 §6, round 1's to settle):
the SSCT name must not be a startable procedure name.** Option B as ruled —
`SSCTSNAM` = jobname[0:4] — makes them equal **by construction** whenever the
jobname is four characters or fewer, which is exactly the production case
(`NSFS`).

## Recovery, and what it cost

The stand was left unable to start NSFS. `jcl/TSTSCTR.jcl` (`PARM='RMNSFS'`)
was written **during the round** to remove an SSCT by name; it removed the stale
registration (`TSTSCTM: RM NSFS rc=0 ABSENT`) and `S NSFS` worked again
immediately. **A round that can create a condition it cannot clear is not
finished**, and without that job the only way back was an IPL.

**Standing hazard while the deployed module carries this code:** any retain-path
stop — or an abend — re-creates the condition. `jcl/TSTSCTR.jcl` is the way out.

## Proof division

- **Host (TSTSCT, 56 assertions):** the pure truth table and the register /
  find / remove plumbing against an in-memory shim. **Not** the SSI mechanism,
  and **not** the real anchor offsets — the shim shares field names, not layout.
- **Cross build:** the real offsets (the MVS half uses the real members) and the
  mirror asserts.
- **Live (TSTSCTM, 24/24):** the mechanism, on the real chain, under two names.
- **Neither:** that two stacks can run — only that the rendezvous distinguishes
  two names; and adoption, which is round 2.

---

## Round state at the hold

**Held on Mike's ruling: nothing committed.** The §3.1 naming rule is reopened
by the finding above, so committing a design that bricks `S NSFS` after any
retain was not done.

**The stand was made safe, not left carrying the hazard.** `P NSFS` clean
(registration removed — PROBE `ABSENT`, `JOB00396`), `NSF.LINKLIB` rebuilt from
`main` **without** the rendezvous hook and redeployed, `S NSFS` restarted. The
revert is confirmed positively: **`NSF060I` is absent**, and that message is
emitted unconditionally by the round-1 module on every successful start (seen
three times during the round), so its absence discriminates rather than merely
failing to appear.

**One cost persists to IPL and is not recoverable:** `LARGEST FREE BLOCK` reads
**929792**, which is 139264 below this stand's 1069056 — the anchor plus router
retained by the deliberate P2 induction (b4's precedent, and the documented
price of exercising the retain branch at all).

## How to resume

Two pieces of temporary machine state were used and have been **undone**, so a
later session must re-create them:

1. **Homebrew `python3` and `node` cannot reach the stand from this Mac** —
   every port, while `/usr/bin/curl` and `/usr/bin/ssh` reach it and python
   reaches the internet and the LAN gateway. System-binary-vs-Homebrew,
   host-scoped: the macOS **Local Network** privacy signature, with the router
   exempted. It worked earlier the same day, so the permission was reset.
2. The workaround was an **ssh port-forward**, chosen so that **mbt and zowe run
   unmodified** — hand-rolling REST calls would have made every result
   unattributable:

```sh
ssh -f -N -o ServerAliveInterval=30 -o ExitOnForwardFailure=yes \
    -L 18080:127.0.0.1:8080 mvsdev
# .env: MBT_MVS_HOST=127.0.0.1, MBT_MVS_PORT=18080   (gitignored; restore after)
# zowe: --host 127.0.0.1 --port 18080 --protocol http
```

**The tunnel drops.** It died mid-round and a `P NSFS` silently did not land;
the tell was `ECONNREFUSED 127.0.0.1:18080` on the next call, and the check that
caught it was `D A,L` showing NSFS still up. `ServerAliveInterval` is in the
recipe above for that reason. **Verify the state after every console command
that matters**, rather than trusting the reply.
