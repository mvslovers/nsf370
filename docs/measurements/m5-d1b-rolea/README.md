# `role_a`'s assertion: assert the documented refusal — live round, 2026-09-03

Small round, one product-source change of zero lines: `test/mvs/tstd1b.c`
(a test), `include/nsfsoc.h` (a comment) and `CLAUDE.md`. No module source
changed, so **`NSF.LINKLIB` was not redeployed** — `make test-mvs` targets
`TESTLIB`, which no STC holds, so §5's `P NSFS → deploy → S NSFS` ordering does
not apply here and one STC instance served the whole round.

Nothing flips. #67, #88 and #92 stay open; #101 stays open and is Mike's.

---

## 1. What was wrong

`role_a` ended with:

```c
rc = nsf_listen(s, 5);
CHECK_EQ((long)rc, (long)NSF_RETOK, "A: its OWN socket still works after B");
```

`tcp_listen` returns `NSF_EINVAL` unless the TCB is `TCP_CLOSED`
(`src/nsftcp.c:1959`, *"only a fresh socket may listen"*), and `role_a`
re-listens with **no intervening close**. So the assertion was **structurally
always-false**: A could never pass.

**It survived because A's CC appears never to have been read.** #100 ran B
after A had already ended, and the d1c record quotes B's counts and not A's.
That is CLAUDE.md §8.5 one level out — not "an absence that looks like a
result", but *a result nobody looked at*. Fixing the assertion without fixing
the reading habit repairs only the symptom, so **A's seven assertions are named
individually below**, not summarised as a CC.

## 2. What replaced it, and why the errno is load-bearing

`rc == NSF_RETERR` alone would be green for **any** failure — the
absent-vs-succeeded shape moved *inside* the assertion. The two refusals are
different statements in `do_listen` (`src/nsfreq.c`):

| errno | meaning |
|---|---|
| `EBADF` (9) | `req_socket` returned NULL — the descriptor did not resolve, **or is not owned by this caller**. Foreign and never-existing are indistinguishable here by construction (ADR-0046), which is exactly why `EBADF` cannot carry this claim. |
| `EINVAL` (22) | the descriptor **resolved**, ownership **held**, the TCB exists and is **not CLOSED**. |

So `EINVAL` is the isolation statement in **positive form**: A's socket
survived B, still belongs to A, and is still listening. `EBADF` here would mean
B had broken the owner — the failure this gate exists for.

The errno read path was **verified, not assumed**: `nsf_listen` puts
`r->errno_` into `g_eza_errno` **unremapped** (`src/nsfeza.c:371`) and
`nsf_lasterrno()` returns it. Writing a new assertion through an unverified
read path would repeat the class §2.1 exists to fix.

## 3. The run

Fresh NSFS (STC 1767) on a post-IPL stand — see
`docs/measurements/post-ipl-2026-09-03.md`. A submitted first; B submitted
**on A's console marker, not on a timestamp** (`TSTD1B: A HOLDING SOCKET
(facade 0) -- B MAY RUN NOW`), which is #101 Stage 2's lesson: matching on
timestamps there cost a run.

### A — JOB03034, **CC 0000, 7/7**, all seven named

```
PASS: the client is UNAUTHORISED
PASS: A: INITAPI across the boundary
PASS: A: SOCKET across the boundary
PASS: A: BIND
PASS: A: LISTEN
PASS: A: a second LISTEN on A's OWN socket is refused          <- new
PASS: A: refused EINVAL -- resolved, owned and still listening (not EBADF)  <- new
```

Before this change A had six assertions, one of them unpassable — 5 PASS /
1 FAIL. **This is the first time A has been green, and the first time its
result has been recorded.**

### B — JOB03035, **CC 0000, 13/13 — unchanged, as required**

```
SWEEP pre-own:  0 reached out of 128 attempts
SWEEP post-own: 1 reached out of 128 attempts, 00010001
PASS: B's OWN socket is inside the swept range (the range is adequate)
PASS: 2.2: owning nothing, B reached NO descriptor (all foreign)
      B's own = 00010001, so A's is derived as 00010000
      foreign(00010000) rc=-1 errno=9 | unknown rc=-1 errno=9
PASS: 2.2b: foreign and never-existing return the SAME retcode / errno
      SELECT poll: rc=1 errno=0 foreign.ready=0 own.ready=2
PASS: 2.3 poll: foreign NOT ready / REST OF MASK SERVED / no error
      SELECT parked: rc=0 errno=0 ready=0
PASS: 2.3 parked: A becoming ready does NOT complete B's SELECT
PASS: 2.3 parked: ...and the entry stays not-ready
```

**The decision is by IDENTITY, not by count** (#100's defect 2): B reached
exactly one descriptor and it was **its own**, `00010001`; A's, derived as
`00010000`, was refused with the same `rc`/`errno` as a never-existing one.

**The range-adequacy check passed**, which is §2.3's point made live: a fresh
STC is what put the live descriptors inside the swept gen window.

### A free corroboration, not designed for here

**`2.3 poll` passed.** It was **red in all three states of both arms** of the
d1c round — #101's `ulen` product defect, constant across the axis and
therefore off-axis. Its passing on a fresh instance is independent confirmation
that the #101 fix is the deployed module's behaviour.

## 4. TWO OF B's THIRTEEN PASSED VACUOUSLY, AND THIS STAND IS WHY

The parked scenario's own comment states the design: *"the host connects to A's
port meanwhile, which makes A's LISTENER read-ready. Check on -> B times out
(rc 0). Check off -> the re-scan completes B (rc 1)."*

**`tun0` does not exist on this stand** (see the post-IPL record), so no host
connect was possible and **A never became read-ready**. B's parked SELECT
therefore timed out at `rc=0 ready=0` — which is precisely what both parked
assertions expect. They would read identically on a build with the ownership
check removed, because the stimulus that discriminates never occurred:

- `2.3 parked: A becoming ready does NOT complete B's SELECT` — **vacuous here**
- `2.3 parked: ...and the entry stays not-ready` — **vacuous here**

The other eleven are not: A genuinely held a live socket (console-confirmed),
B genuinely derived and was refused A's real descriptor, and the poll path ran
against it.

**This round did not make the parked path worse and did not improve it** — d1's
own first round already recorded that §2.3's parked path "was never driven at
all". Driving it non-vacuously needs the CTCI pair, hence a Hercules restart;
that is not this PR's scope. **Recorded rather than left for someone to
discover from a green matrix**, which is the whole of §8.5.

## 5. Deploy took effect — positively, not by absence

The kickoff expected the fingerprint to be an absence ("this change alters no
error set"). It is not: **the new assertion's TEXT is the marker.** The old
module's string was `A: its OWN socket still works after B`; it cannot produce
`A: a second LISTEN on A's OWN socket is refused`. Both new strings are in the
spool, so the new binary ran.

Checked before deploying, too: the new text was confirmed present in
`build/TSTD1B` **in EBCDIC** (`iconv -t IBM-1047`), because `make test`'s log
listed only `tstrqxc.c` and a build that reports "56 modules" while rebuilding
nothing looks identical to one that did the work.

## 6. Hygiene, and the second free-block reading

- `P NSFS`: `NSF043I SVC 239 RESTORED`, `NSF044I TRANSPORT STOPPED`,
  `NSF853I SWAPPABLE AGAIN`, `NSF011I`, `IEF404I` — **no `NSF054W`**, so the
  drain completed and the CSA went back.
- `S NSFS` after it: `NSF055I ... LARGEST FREE BLOCK NOW **1073152**`,
  **identical to the first start**, and on the **same anchor `00A8B7C8` and the
  same router EP `00A820C8`**. A retained anchor forces a *different* address,
  so this is the leak check in its strongest available form: the run left
  nothing behind.
- **Zero dumps** (`IEA995I` count 0) and **zero abends** (`IEF450I` count 0)
  across the entire round — and here the absence is readable, because the round
  contains no deliberate cancel that would supply a positive control.
- `TSTD1B` deployed with `--only`, which **replaces** TESTLIB. Its no-PARM run
  in the matrix reads `FAIL CC 20` — the documented "could not run" third
  state, not a failure.

## 7. What this does NOT establish

- **The parked SELECT path** (§4) — still undriven.
- **Anything in Phase 1.** Both ownership checks take the zero-identity red
  line, so `NSF` is unaffected and was not redeployed.
- **Anything about `role_b`'s code**, which was not touched.
- The generation-window sentence added at `include/nsfsoc.h` `soc_desc` is a
  **documentation** change: it records a property of the `(gen<<16)|id`
  encoding that every future sweep-based check inherits. It is not a soundness
  fix — `tstd1b.c`'s sweep already detects the condition and refuses to report,
  and has done so live twice. The cost of the missing sentence was a burned
  run, never a wrong result.
