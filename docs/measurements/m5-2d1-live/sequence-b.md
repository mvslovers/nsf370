# Second live round — the stand sequence, written BEFORE touching the stand

The first round put `TSTD1BA` into `ABEND SFEF` because `P NSFS` was issued inside
A's 60 s hold window. That was sequencing, and it is avoidable by writing the
order down first. This is the order.

**Invariant: never stop NSFS while an A job is holding.** A's hold is 60 s; every
`P NSFS` below happens with no A job in flight, checked with `D A,L` first.

1. Offline: repair instruments, add the R8 client, cross-build, host suite.
2. `P NSFS` → `make deploy` (product unchanged, but TESTLIB clients changed) →
   `S NSFS`.  *(no A holding: nothing has been submitted yet)*
3. Deploy TESTLIB with TSTD1A + TSTD1B + TSTD1R.
4. **§2.4** — TSTD1R, single job, no partner, no hold.
5. **§2.2b / §2.3** — submit A, wait for its console readiness line, submit B
   inside the window, wait for **B DONE and A ENDED** before anything else.
6. **§2.5 arm 1** (descriptor check off): confirm no A in flight → P NSFS →
   deploy → S NSFS → run 5 again → restore → deploy → S NSFS → run 5 again.
7. **§2.5 arm 2** (R8 check off): same shape, run 4 instead of 5.
8. **§2.6** — Phase 1 last, because it needs the CTCI pair: confirm nothing in
   flight → `P NSFS` → `S NSF` → checks → `P NSF` → `S NSFS`.
9. Leave the stand on the restored build, NSFS up, and verify
   `BUSY=0 INFLIGHT=0`, zero dumps, no `NSF054W`.
