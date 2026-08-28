# M5-79 — the live gate for issue #79

*Stand: MVSCE on `mvsdev`, real CTCI pair 0500/0501. All figures below are from
the round of 2026-08-28 (guest clock ~7 h behind CEST). Host baseline
**2991 PASS / 0 FAIL**, confirmed before and after.*

The audit is `audit.md`; this is what the machine said.

---

## 0. The result in one table

| arm | module | abend | `NSF903I` | `S NSFS` afterwards |
|---|---|---|---|---|
| **B** | fix + injection (startup) | S0C4 | **yes** | **SUCCEEDS** — `NSF042I SVC 239 STOLEN (EP 00A8B248)` |
| **B (again)** | same | S0C4 | yes | SUCCEEDS |
| **B2** | fix + injection **inside `evt_mainloop`** | S0C4 | yes | SUCCEEDS |
| **C** | **fix reverted**, same injection | S0C4 | **no** | **FAILS** — `NSF049E SVC 239 IN USE (EP 00AF3248) -- NOT STOLEN` |
| **D** | fix + injection + **forced `__super` failure** | S0C4 | `NSF904E` | **FAILS** — `NSF049E SVC 239 IN USE (EP 00A820C8) -- NOT STOLEN` |

**Exactly one assertion moves**, and the axis that moves it is the one the fix
names. #79's own field observation (STC 1505/1506 on `main`, a *spontaneous*
S0C4) is the fourth, uncontrolled data point and agrees with arm C.

---

## 1. The environment reading — the state question, answered

```
NSF902I RECOVERY ENVIRONMENT: SUP=N AUTH=Y
```

Identical on **five** separate abends across **three** different modules
(STC 1540, 1541, 1542, 1543). So an ESTAE exit on this system is entered:

* **problem state** — which is why `nsfsx_recover_quiesce` must borrow key 0 at
  all, and why the `__prob` restore (not `__super`) is the correct return leg.
  The `__issup()` capture is therefore live-confirmed as taking the branch it
  was written for, and the supervisor-entry branch is **unexercised** — kept
  because the cost is one compare and the failure it prevents (returning to RTM
  in a state it did not ask for) is silent.
* **APF-authorised** — `__super`'s precondition (`__isauth()`) holds, which is
  what makes the slot restore possible at all. Assumed at design time on
  "the exit runs on the same TCB"; now measured.

---

## 2. Arm B — the fix, and what recovery left behind

```
NSF042I SVC 239 STOLEN (EP 00A820C8)
NSF041I NSFS TRANSPORT READY -- ANCHOR=00A8B7C8 ECB=000BE0D4
NSF001I NSFS INITIALIZATION COMPLETE
NSF900E NSF ABEND INTERCEPTED -- CAPTURING AND PERCOLATING
NSF902I RECOVERY ENVIRONMENT: SUP=N AUTH=Y
NSF903I NSFS TRANSPORT QUIESCED BY RECOVERY -- SVC RESTORED, CSA AND ROUTER RETAINED
IEA700I A0A-  2 NSFS     NSFS     00 0000A040 0015DFC0 009DC7B0
IEF450I NSFS NSFS - ABEND SA0A U0000
```

and then, **on the same IPL**:

```
NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144) -- LARGEST FREE BLOCK NOW 933888
NSF042I SVC 239 STOLEN (EP 00A8B248)
NSF041I NSFS TRANSPORT READY -- ANCHOR=00AAD7C8
NSF001I NSFS INITIALIZATION COMPLETE
```

That second `NSF042I` is the whole issue: it is the line #79 says cannot
happen. A **new** router EP (`00A8B248`, was `00A820C8`) and a **new** anchor
confirm the previous ones were retained, not reused.

**The retained CSA is 139 264 bytes**, exactly as #79 measured: largest free
block `1073152` → `933888`. Retained on purpose (`audit.md` §3), and named in
the message rather than left to be discovered.

### 2.1 Read back from the retained anchors — the two writes, and one bit

The anchors survive the address space, so the fix can be checked directly
rather than trusted. Four anchors, same layout, read through `/.dm` **after**
their STCs were gone:

| anchor | arm | `flags` +0C | `server_ecb_ptr` +24 |
|---|---|---|---|
| `00A8B7C8` | B run 1 | `00000000` | `00000000` |
| `00AAD7C8` | B run 2 | `00000000` | `00000000` |
| `00ACF7C8` | B2 (mainloop abend) | `00000000` | `00000000` |
| `00AF37C8` | **C — fix reverted** | **`80000000`** | **`000BE0D4`** |

`80000000` is `NSFV_ANCHOR_ACTIVE`. `000BE0D4` is precisely the address STC
1543 published at startup (`NSF041I ... ECB=000BE0D4`) — **a live pointer into
the private storage of a dead address space, left standing**, which is the
hazard `audit.md` row 5 exists for. Everything else is identical across all
four (`NSFVANCR`, `version=3`, `nslots=0x40`), so the comparison isolates
exactly the two words `nsfsx_recover_quiesce` writes and nothing else.

### 2.2 `NDS=1` — the `OKSWAP` question, answered from the restart

Every start, including each one after an abend, reported:

```
NSF851I NSFS NON-SWAPPABLE (SYSEVENT DONTSWAP, NDS=1)
```

`NDS` is a **count**. A `DONTSWAP` surviving the abended address space would
have made the next instance read `2`. It read `1` every time, which is
64-3-1's fresh-STC baseline. **Row 6 confirmed live: nothing carries over, so
`OKSWAP` on the recovery path would be a no-op** — matching the structural
argument (the OUCB is reached through the ASCB, which is freed with the address
space). Reasoned in `audit.md` §4, measured here.

---

## 3. Arm C — the revert, and the gate

The revert removes `nsfsx_recover_quiesce()` and its message switch **and
nothing else**: `NSF902I` stays, the injection stays, the ordering stays. Diff
verified to be exactly that before deploying.

```
NSF900E NSF ABEND INTERCEPTED -- CAPTURING AND PERCOLATING
NSF902I RECOVERY ENVIRONMENT: SUP=N AUTH=Y          <- no NSF903I
IEA700I A0A-  2 NSFS ...
```

then:

```
NSF049E SVC 239 IN USE (EP 00AF3248) -- NOT STOLEN
NSF009E NSFS TRANSPORT INITIALIZATION FAILED
IEF450I NSFS NSFS - ABEND SA0A U0000
```

Byte-for-byte the failure #79 reports, reproduced under a **controlled**
stimulus rather than a spontaneous one.

**Deploy-took-effect check, positive in both directions** (§5's most expensive
failure class): `NSF903I` present ⇒ the fixed module is running; `NSF903I`
absent **together with** `NSF049E` ⇒ the reverted one. Neither reading is
available on the other module, so no arm rests on an absence alone.

---

## 3a. Arm D — the `__super`-fails posture, demonstrated

Run after IPL #1, on a clean slot. The failure is forced the **64-3-1 way** —
`if (1 || __super(PSWKEY0, &savekey) != 0)` short-circuits, so `__super` is
**never called** and the task never enters supervisor key 0. Inverting the
comparison instead would have entered key 0 and returned without `__prob`,
leaving the task authorised; this way the slot is **genuinely** not restored,
which is the posture under test rather than a message rehearsal.

```
NSF042I SVC 239 STOLEN (EP 00A820C8)
NSF001I NSFS INITIALIZATION COMPLETE
NSF900E NSF ABEND INTERCEPTED -- CAPTURING AND PERCOLATING
NSF902I RECOVERY ENVIRONMENT: SUP=N AUTH=Y
NSF904E NSFS SVC SLOT STILL STOLEN (RC=2) -- NSFS CANNOT RESTART, AN IPL IS REQUIRED
NSF901I NSF EMERGENCY TEARDOWN COMPLETE
IEF450I NSFS NSFS - ABEND S0C4 U0000
```

then, confirming the failure was real and not cosmetic:

```
NSF049E SVC 239 IN USE (EP 00A820C8) -- NOT STOLEN
NSF009E NSFS TRANSPORT INITIALIZATION FAILED
```

Four things are demonstrated, and the last two were not the point but matter
more than the first two:

1. **The message fires with DEFINED values** — `RC=2` is `NSFSX_RQ_NOKEY`, not
   a stack remnant. This is the class 64-3-1 caught by re-reading code
   (`swap_set` returning before writing `*out` on its own `__super` arm); the
   equivalent hole does not exist here, and now it is measured rather than
   argued.
2. **It NAMES THE CONSEQUENCE.** An operator does not have to deduce that the
   next `S NSFS` will fail — and the very next line of the log is that failure.
3. **The warn-and-continue pin holds.** `NSF901I` follows: recovery did not
   hang, loop or bail — it reported and finished. Same posture as `NSF852W`
   (ADR-0044 §8), now shown for the recovery path.
4. **The ORIGINAL abend code percolates.** `IEF450I ... ABEND S0C4`, not a
   code manufactured by the exit — the ESTAE contract doing what it should.

---

## 4. A SECOND DEFECT, FOUND BY THE GATE AND NOT INTRODUCED BY IT: recovery does not reach `NSF901I`

Every abend in this round ended:

```
IEA700I A0A-  2 NSFS     NSFS     00 0000A040 0015DFC0 009DC7B0
IEF450I NSFS NSFS - ABEND SA0A U0000
```

with **`NSF901I NSF EMERGENCY TEARDOWN COMPLETE` missing**. `NSF901I` is
emitted immediately after `nsf_shutdown()`, so `nsf_shutdown()` is taking a
second abend inside the exit. `SA0A` is the SVC 10 (GETMAIN/FREEMAIN) abend
family, and `mm_shutdown()` releases its pool regions with `free()` — libc370
heap, hence `FREEMAIN`. The `IEA700I` operands are **byte-identical across all
five occurrences**, so it is deterministic, not a corruption artefact.

**It is not caused by this change, and the round establishes that three ways:**

1. **Arm C shows it with `nsfsx_recover_quiesce` removed** — same
   `A0A- 2 … 0000A040 0015DFC0 009DC7B0`. The quiesce, its key window and its
   two CSA stores are excluded.
2. **Arm B2 shows it at #79's own lifecycle point.** The suspicion that the
   startup-path injection abends "too early" — before `nsftmr_plat_arm` and
   before `evt_mainloop` — was tested by moving the injection into
   `nsfsx_stats_extra`, i.e. onto the executive task **inside the mainloop**,
   reached by `F NSFS,STATS`. The STC came up clean, served the operator, and
   then produced the identical `A0A`. Lifecycle position excluded.
3. **`main` already did this on a neighbouring path.** 40-CHK's own record
   (`docs/measurements/40-chk/findings.md:128`) has `main`'s refuse-to-start
   path — `NSF009E` then `nsf_shutdown()` then `return 8` — abending
   `IEF450I NSFS NSFS - ABEND SA0A U0000`. This round reproduced that line
   byte-identically in arm C run 2. So "`nsf_shutdown()` then terminate" was
   already abending `A0A` on `main`, on a path nobody was reading it on.

**What is NOT established: why — and arm D moved the question rather than
answering it.** Arm D, run after IPL #1, is the **first arm with no `A0A`**:
`NSF901I` was emitted and the original `S0C4` percolated cleanly. But it is
**confounded two ways** and cannot be read as a cause:

* it short-circuits the quiesce **before** `__super`, so no key window ran; and
* the CTCI pair was still offline after the IPL (`NSF202E CTCI 0500 ALLOC
  FAILED S99 ERR 0244`, `NSF212E CTCI 0500 FAILED TO START`), so the device
  **I/O subtasks were never attached**.

Across all six observations the device state is the variable that tracks the
`A0A` perfectly — four arms with `NSF210I/NSF211I` all produced it, two arms
without devices produced none — and there is a mechanism to fit: the recovery
path does **not** quiesce devices (`audit.md` row 7), so `mm_shutdown()`'s
`free()` runs while two ATTACHed subtasks are still live on the same
non-reentrant libc370 heap. The clean path cannot hit this because
`dev_foreach(nsf_quiesce_device)` runs *before* `nsf_shutdown()`.

**But that hypothesis has a counter-example already on record, and it is the
reason this is not being asserted.** 40-CHK's STC 1505 abended on the
**RECVFROM completion path** — a datagram had arrived over the CTCI, so the
devices were up and the subtasks live — and it reached `NSF901I`. Devices-up
therefore does *not* imply `A0A`.

So two candidates stand, neither excluded: **the device subtasks**, and
**`NSF902I`** (added by this change, present in every A0A arm and absent from
`main`'s STC 1505). The controlled arm that separates them is **free** — the
fixed module restores the slot, so it costs no IPL — and it is one binary with
one variable: run arm B with the CTCI **offline**, then bring the pair online
and run it again. That is the next step, and `audit.md` §5's "rows 7 and 8,
named and deliberately not fixed here" may need revisiting depending on how it
reads.

**What it costs: nothing that matters, and this is why it is reported rather
than fixed in this round.** The NSFMM pool regions are **AS-scoped**
(`audit.md` §1) — MVS reclaims the private region as the address space
terminates, so a `free()` that never runs loses nothing. Critically, the `A0A`
happens **after** `NSF903I`: the SVC slot is already restored, the anchor
already marked inactive. **It does not touch the fix's purpose**, which is why
arm B restarts cleanly in spite of it. It is a *reporting* defect — recovery
ends without saying it finished, and an operator sees an unexplained second
abend code instead of `NSF901I` — and it also means any teardown step added to
`nsf_shutdown()` in future will silently not run in recovery. **Filed as issue
#83.**

---

## 5. Housekeeping and stand state

Four retained anchor+router pairs accumulated across the round (three from the
arm-B family, one from arm C), plus one failed start. Largest free CSA block
went `1073152` → **`487424`**. This is the designed retain posture doing its
job, at the rate the gate demanded of it; **an IPL is the only way to reclaim
it** ([[nsf370-mvsdev-ipl]]).

**The stand is left with `SVC 239` stolen by arm C's dead STC** — that is the
defect, deliberately reached, and it is what an IPL clears. No NSF client may
be run in this state.

Zero dumps: `SYSUDUMP` never engaged (`IEA700I` is the no-dump form).
