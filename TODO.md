# Open Work, Ranked

Holds order and rationale only. The GitHub issues are the source of truth for
each item; CLAUDE.md §7 carries the one-line milestone status and
`docs/milestone-history.md` the per-round record. Update this file after every
commit, push or merge that moves an item (root CLAUDE.md, Git Workflow).

## M5 — Phase 2

1. **TCP hardening + docs** — the rest of M5's scope; no issue yet.
2. **d2 ESTAE retain round** (ADR-0048) — its own round with predictions
   (maintainer, 2026-10-15): an abend with a parked client, the recovery
   retains, the next start adopts and frees. Unblocked by #83: recovery now
   completes with the devices up (ADR-0049). The #83 round already saw two
   recovery-retained anchors (inflight 0) adopted and freed, ungated. No issue
   yet.
3. **#88 client liveness** — not soundly detectable on MVS 3.8j; the real
   design behind the best-effort sweep, and the reason UNKNOWN owners stay out
   of d2's scrub.
4. **c3 — retire the probe verbs** (`ECHO`/`XFER`/`UNSTAGE`/`SLOT`, promote
   `QUERY`) once their eight dependent live gates have replacements. No issue
   yet.
5. **#87** no test drives the inline rxq-dequeue completion or a cross-AS
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
- PR #127 (libc370#448: one C startup in `libc.a`, plus `crtm.o`) — open since
  2026-10-05 with CI red from a run before the mbt 3 migration. It edits the
  Spec and the frozen Project Brief. Needs a decision: rebase onto `main`, or
  close.

## Done recently

- ~~#83 A0A inside the recovery exit~~ — PR #139 (ADR-0049); live-green on
  dev1. Not established on MVS: Phase-1 recovery, the `NSF206W` → `NSF068W`
  path.
- ~~d2 round 2, adoption (#136)~~ — PR #138; live-green on dev1 after round 2b.
  Not established live: a real client death in a retained anchor, S2,
  ASIDE, BADVER and the ESTAE retain path.
- ~~#129 MVCK mnemonic~~ — PR #134.
- ~~#132 TSTVSNP pins C99 truncation~~ — PR #133.
- ~~mbt 3 migration + libc370 2.x port~~ — PRs #130, #131.
- ~~d2 round 1~~ — PR #135.
