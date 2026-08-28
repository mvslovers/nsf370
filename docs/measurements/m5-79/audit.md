# M5-79 — the Phase-2 resource audit

*Enumerated from source on branch `m5-79-recovery-teardown`, base `main` @ `4d5185d`.*

Issue #79 was reported as a missing call. It is not. The finding this audit
exists to record is:

> **Every resource Phase 2 acquired was added to the clean teardown and to
> nothing else.** Phase 1 is unaffected — `nsf_shutdown()` was *complete* when
> it was written. Nothing broke; it stood still while obligations accumulated
> underneath it.

The fix is the easy half. The table is the deliverable that stops this
recurring, and the column that matters is the last one.

---

## 1. The dividing line that organises the table

An ESTAE exit percolates: the address space terminates immediately afterwards.
So the question for each resource is not "is it released" but **"does anything
other than us release it"**, and there are exactly two answers:

* **AS-scoped** — MVS reclaims it at address-space termination. Recovery need
  not touch it, and in a damaged environment *should* not: every extra step is
  a fresh chance to fault before the one step that matters.
* **System-scoped** — it lives in **common storage or the nucleus**, so it
  survives the address space. **Nobody reclaims it but us.**

Only the system-scoped rows can cost an IPL, and there are three of them.

---

## 2. The table

| # | Resource | Acquired at | Scope | Clean path | Recovery **before** | Recovery **after** |
|---|---|---|---|---|---|---|
| 1 | **SVC 239 slot** (nucleus `SVCTABLE`) | `nsfsx_svc_steal` ← `nsfsx_start` | **system** | `nsfsx_stop` :452 — unconditional, both branches | ❌ **none** — the blocker | ✅ `nsfsx_recover_quiesce` |
| 2 | **Router module** (`__loadhi` into CSA) | `nsfsx_router_load` | **system** | `nsfsx_stop` — **drained branch only** | ❌ none | ❌ **deliberately none** (§3) |
| 3 | **CSA anchor**, 137 272 B `SP=241` | `nsfsx_anchor_alloc` | **system** | `nsfsx_stop` — **drained branch only** | ❌ none | ❌ **deliberately none** (§3) |
| 4 | `ANCHOR_ACTIVE` flag | `nsfsx_anchor_alloc` | in (3) | cleared by `nsfsx_stop` | ❌ none | ✅ cleared |
| 5 | `server_ecb_ptr` — published address of **STC-private key-8** storage | `nsfsx_start` | in (3) | nulled by `nsfsx_stop` | ❌ none | ✅ nulled |
| 6 | **DONTSWAP** (SRM, `OUCBNSW`/`OUCBNDS`) | `nsfswap_dontswap` :258 | **AS** | `nsfswap_okswap` :469 | ❌ none | ❌ none — **correct**, §4 |
| 7 | Device channels, **SVC 99** allocations, I/O buffers | `dev_start` → `ctci_chan_alloc` | **AS** | `dev_foreach(nsf_quiesce_device)` :453 | ❌ none | ❌ none — a finding, not this diff (§5) |
| 8 | I/O subtasks (`nsfthr` / cthread) | `dev_start` | **AS** | via `dev_shutdown` | ❌ none | ❌ none — §5 |
| 9 | NSFMM pool regions | `mm_init` + pool builds | **AS** | `nsf_shutdown` | ✅ `nsf_shutdown` | ✅ unchanged |
| 10 | STIMER heartbeat / async exit | `nsftmr_plat_arm` :441 | **AS** | `nsf_shutdown` → `nsftmr_plat_disarm` | ✅ | ✅ unchanged |
| 11 | ESTAE itself | `__estae(ESTAE_CREATE)` :406 | **AS** | `__estae(ESTAE_DELETE)` :475 | n/a — RTM owns it on percolate | n/a |
| 12 | Task APF authorisation (SVC 244) | `clib_apf_setup` :240 | **task** | dies with the task | n/a | n/a |
| 13 | Trace ring / stats registry | `nsftrc_init` / `sts_init` | static | nothing to release | n/a | n/a |

Line numbers are `src/nsfsmain.c` on `main`.

**Rows 1, 2 and 3 are the whole of the system-scoped set.** Row 1 is what #79
measured; rows 2 and 3 are the ~139 KB that leaks with it (`NSF055I … LARGEST
FREE BLOCK NOW` `1073152` → `933888` across one abend, and an IPL put it back
exactly).

---

## 3. Why rows 2 and 3 stay leaked, on purpose

Recovery restores the slot and **retains** the anchor and the router. That is
not an omission, it is M5-2b3's rule applied unchanged:

> A client that failed to drain is parked in a `WAIT` **inside that code**,
> supervisor state, key 0. Freeing the module pulls the instructions out from
> under a task that is going to resume on them. Retaining the anchor while
> freeing the code is *strictly worse* than leaking both.

An exit cannot know whether a client is parked — it must not drain to find out
(§6) — so it takes the retain posture **unconditionally**. Restoring the table
entry is a **different act** from freeing the module, and only the first is in
scope. Leaking common storage until an IPL is the cheap side of the same
safe-side asymmetry the death guard is built on.

The distinction row 1 turns on: with the slot restored, no *new* client can
enter, so the retained module is unreachable from outside — it is storage, not
an active surface.

---

## 4. Row 6 — `OKSWAP` at recovery: the answer is *no*, with evidence

The kickoff asked whether the clean path's `nsfswap_okswap()` :469 is a second
leak. It is not, and the reason is structural.

`nsfswap_read` reaches the OUCB **through the ASCB** — eyecatcher plus the
`OUCBASCB` back-pointer, `src/nsfswap.c`. **The ASCB is freed at address-space
termination, and SRM's OUCB with it.** There is nothing left to release and
nothing to issue `SYSEVENT` *against*: the non-swappable attribute cannot
outlive the address space that holds it. Adding a `SYSEVENT` to a damaged
environment would buy nothing and cost a step.

**Live corroboration is free**, and falls out of the arm that is being run
anyway. 64-3-1 established that a fresh STC baselines at `NDS=0` and reports
`NDS=1` after its own `DONTSWAP`. So on the successful restart after an abend:

* `NSF851I … NDS=1` ⇒ nothing carried over. Row 6 confirmed.
* `NDS=2` would mean the previous instance's `DONTSWAP` survived — the leak.

Recorded in `findings.md` with the number read off the restart.

---

## 5. Rows 7 and 8 — named, and deliberately not fixed here

Devices are the row it is tempting to fix, and it is the Kitchen-Sink line.
SVC 99 dynamic allocations and ATTACHed I/O subtasks are **address-space
scoped**: MVS unallocates and terminates them when the address space goes. The
clean path quiesces them for a different reason — it runs *before* the address
space ends, while the pools still exist, so it must return storage in order.
Recovery has no such ordering problem and no such need.

They are in the table because the audit's value is the enumeration, not the
diff. If a future Phase-2 resource turns out to be system-scoped, this is the
column it must be entered in.

---

## 6. What recovery must not do, restated as rules

* **No drain.** `nsfsx_stop`'s drain polls up to 10 s (`NSFSX_DRAIN_MAX`) and
  nudges parked clients. A polling loop in an exit in a damaged environment is
  not acceptable.
* **No free, no unload.** §3.
* **No `OKSWAP` by reflex.** §4.
* **Slot restore first**, before `nsf_shutdown()`: `mm_shutdown()` releases
  every pool region, so anything touching NSF state afterwards reads freed
  storage — and the one action whose absence costs an IPL belongs ahead of
  every later step that could fault.
* **Report on the flag, not on having run.** `nsfsx_svc_restore_locked` is a
  no-op when the slot was never stolen; "it did not need doing" must not read
  the same as "it was done" (CLAUDE.md §8.5). Hence four distinct return
  values, and `NSF903I` / `NSF904E` as *different messages* rather than one
  message and a silence.

---

## 7. The silent-failure defect this round had to fix first

`nsfsx_svc_restore` could not report failure:

```c
if (!g_svc_stolen || !g_svc_slot) return;
if (__super(PSWKEY0, &savekey)) return;      /* silent */
...
wtof("NSF043I SVC %u RESTORED", ...);
```

On a `__super` failure it returned having done nothing and said nothing —
CLAUDE.md §8.5 in pure form, and it made the acceptance item *"demonstrate the
`__super`-fails posture"* **unsatisfiable**: there is nothing to assert through
a function that cannot tell you which of the two things happened.

Fixed without disturbing the countersigned clean path: the table write is
factored into `nsfsx_svc_restore_locked()` (key 0 assumed, no message), and
**both** callers share that one encoding. The recovery caller tests
`g_svc_stolen` afterwards and turns it into `NSFSX_RQ_QUIESCED` or
`NSFSX_RQ_STUCK`. `nsfsx_svc_restore`'s own behaviour and message are
byte-for-byte what they were.

---

## 8. The state question, and why the code does not assume the answer

`__super` skips `MODESET MODE=SUP` when the task is already supervisor.
**`__prob` MODESETs to problem state unconditionally.** So the ordinary
`__super`/`__prob` pair would drop an exit that was *entered* supervisor into
problem state on the way back to RTM — a state change RTM did not ask for, on
the percolate path.

`nsfsx_recover_quiesce` therefore captures `__issup()` at entry and returns the
task to what it found (`__super(savekey, NULL)` keeps supervisor and restores
the key; `__prob(savekey, NULL)` restores both). **The entry state is measured,
not assumed** — `NSF902I RECOVERY ENVIRONMENT: SUP=… AUTH=…` is emitted on
every abend, one WTO on a path that by definition already has the operator's
attention, and it is what decides whether the slot restore is possible at all.
`__super` additionally requires `__isauth()`; the exit runs on the same TCB, so
it should hold, and `AUTH=` is the half of the reading that says whether it did.
