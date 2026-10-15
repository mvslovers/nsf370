# Open Work, Ranked

Holds order and rationale only. The GitHub issues are the source of truth for
each item; CLAUDE.md §7 carries the one-line milestone status and
`docs/milestone-history.md` the per-round record. Update this file after every
commit, push or merge that moves an item (root CLAUDE.md, Git Workflow).

## M5 — Phase 2

1. **d2 round 2: adopt a retained anchor at start** (#136, ADR-0048). Built
   on branch `d2-round2` (PR #138, not yet on main) and **live-green on dev1**
   after round 2b fixed round 1's unwritten `jobname_tail`
   (`docs/measurements/d2-round2/`, `d2-round2b/`). What remains: merge PR
   #138, which closes #136. Not established live: a real client death in a
   retained anchor, S2, ASIDE, BADVER and the ESTAE retain path. Why first:
   every NSFS abend today leaves about 139 KB of CSA until IPL.
2. **TCP hardening + docs** — the rest of M5's scope; no issue yet.
3. **#83 `A0A` inside the recovery exit** — `nsf_shutdown()` takes a second
   abend inside ESTAE, so recovery never reaches `NSF901I`.
4. **#88 client liveness** — not soundly detectable on MVS 3.8j; the real
   design behind the best-effort sweep, and the reason UNKNOWN owners stay out
   of d2's scrub.
5. **c3 — retire the probe verbs** (`ECHO`/`XFER`/`UNSTAGE`/`SLOT`, promote
   `QUERY`) once their eight dependent live gates have replacements. No issue
   yet.
6. **#87** no test drives the inline rxq-dequeue completion or a cross-AS
   SELECT.

## Small

- #35 MIH restarts on the parked CTCI READ pollute `ierr`.
- #53 `tstmvcd.c` 2.2b flaky (false negatives only).
- #54 `tstmvck.c` scenario 3 restores a hardcoded key.
- #56 `UNSTAGE` early-returns on a FREE slot.
- #120 entry CSA reading moved three times; one movement explained. No
  absolute free-CSA figure is quoted until it is.

## Waiting on the maintainer

- #92 STATS truncation — fixed in PR #95; closing is Mike's.
- #59 SVC table readable from key 8 — a measurement, kept open as a record.

## Done recently

- ~~#129 MVCK mnemonic~~ — PR #134.
- ~~#132 TSTVSNP pins C99 truncation~~ — PR #133.
- ~~mbt 3 migration + libc370 2.x port~~ — PRs #130, #131.
- ~~d2 round 1~~ — PR #135.
