# NSF — M5 Status and Open Items

The prose status of the running milestone and its open items. It used to sit in
`CLAUDE.md` §7 under the milestone table; it moved here verbatim on 2026-10-19 so
`CLAUDE.md` keeps one status line per milestone. A round updates this file and the
one-line status in `CLAUDE.md` §7, and appends its record to
`internals/milestone-history.md`. The ranked order of open work is `TODO.md`; the GitHub
issues stay the source of truth.

---

**M5 — where it stands.** Stage-0 probes done (private SVC 239 transport
ADR-0038, keyed CSA bounce ADR-0039, client-death guard ADR-0040). **M5-2, the
Phase 2 request crossing, is COMPLETE** (PR #121): NSFRQE copied into a CSA slot
and dispatched from an STC-private copy (ADR-0041); 64-slot `CS` pool (ADR-0042);
wake contract (ADR-0043); DONTSWAP (ADR-0044); best-effort app sweep (ADR-0045);
descriptor ownership at the request boundary (ADR-0046); `ubuf`/`ulen`
transport-owned bytes (ADR-0047). Gated by Job A (`TSTRQX2`, two clients, A 18/18
B 23/23; wire arm since run, PR #119) and Job B (`TSTRQXC` baseline, three valid
runs; two clients reach 2.38–2.42× one client, **unexplained**). All four
write-out key categories are closed.

**Open — M5:**
- **d2 — UNKNOWN at shutdown / the stranded retained anchor:** the only named
  unproven property of M5-2. ADR-0048 (PR #125) decides to adopt a retained
  anchor at the next start via a per-instance SSCT. **Round 1** (register /
  find / remove; the SSCT name is jobname[0:4] in lower case, so `S NSFS`
  never meets it) is live-green on dev1 (`internals/measurements/d2-round1b/`).
  **Round 2** (adoption, #136; anchor version 4) is **live-green on dev1
  after round 2b**: round 1 never wrote `jobname_tail`, so round 2's first
  live adoption read FOREIGN; register now stamps it, and a start adopted an
  anchor NSFS itself retained (`internals/measurements/d2-round2b/`). Both reaps
  were of a synthetic death.
- **c3 — retire the probe verbs** (`ECHO`/`XFER`/`UNSTAGE`/`SLOT`; promote
  `QUERY`) only once their eight dependent live gates have replacements.
  Obligation #4 stays half-discharged until then.
- **TCP hardening + docs** — the rest of M5's scope.
- Closed on GitHub: #67 (probe-verb slot stranding), #101 (`ulen` as bytes) and
  **#64** (NSFS mid-swap-out stall) — but the record calls #64 *mitigated by
  DONTSWAP (ADR-0044), not fixed*; 64-1's wake-ECB reset may be the fix (weak
  evidence). Do not cite it as fixed.
- No figure from (e) may be quoted without its conditions (fresh host reboot,
  one emulator, restored task set, known start order); no absolute free-CSA
  figure is quoted (#120).

**Open issues and their order live in `TODO.md`** (ranked, with the reason for
each rank); the GitHub issues stay the source of truth (`gh issue list`).

Critical path was M0-1 + M0-2; M5 carries the only real schedule risk and is
isolated so M0–M4 already deliver a usable in-process stack.
