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

**The cause is narrowed by a controlled arm, and `NSF902I` is EXCLUDED.** After
IPL #2 the CTCI pair came back **offline** (`NSF202E CTCI 0500 ALLOC FAILED S99
ERR 0244`), which handed the round a free single-variable experiment: **one
binary** — arm B, the fix plus the same forced `S0C4` — run with the devices up
and with the devices down.

| arm B run | CTCI | `A0A` | `NSF901I` |
|---|---|---|---|
| ×3, before the IPLs | **up** (`NSF210I`/`NSF211I`) | **yes** | no |
| ×1, after IPL #2 | **down** | **no** | **yes** |

Devices down, the same recovery path ran in full and finished:

```
NSF903I NSFS TRANSPORT QUIESCED BY RECOVERY -- SVC RESTORED, CSA AND ROUTER RETAINED
NSF901I NSF EMERGENCY TEARDOWN COMPLETE
IEF450I NSFS NSFS - ABEND S0C4 U0000
```

That **excludes `NSF902I`** — it ran here — and it excludes the key window a
second time, more strongly than arm C did: arm C only showed the `A0A` surviving
the quiesce's *removal*, whereas this arm runs `nsfsx_recover_quiesce` in its
entirety (`__super`, both CSA stores, `__prob`) and does not fire. Arm D agrees
from the other side, though it is confounded (devices down **and** the quiesce
short-circuited), which is why it is not the evidence being leaned on.

**The mechanism that fits is `audit.md` row 7.** Recovery does **not** quiesce
devices: the clean path runs `dev_foreach(nsf_quiesce_device, NULL)` *before*
`nsf_shutdown()`, and `nsf_recover` goes straight there — so `mm_shutdown()`'s
`free()` runs while two ATTACHed CTCI I/O subtasks are still live on a libc370
heap that is not reentrant across subtasks. `SA0A` is the SVC 10 family, which
is what a corrupted free-chain surfaces as.

**Necessary, not sufficient — and that is evidence FOR a race.** 40-CHK's STC
1505 abended on the RECVFROM completion path with devices up and subtasks live,
and reached `NSF901I`. So devices-up does not *always* fire it. A race fires
sometimes; a deterministic bug does not. (The `IEA700I` operands being identical
across all five firings says the *failing free* is the same one each time, not
that the race always fires.)

**So `audit.md` §5 needs reading with this next to it.** Rows 7 and 8 were
entered as "named, and deliberately not fixed here" on the grounds that MVS
reclaims them anyway. That is still true of the *storage*, and it is why the
`A0A` costs nothing — but the row is no longer merely cosmetic: **it is the
leading candidate cause of a second abend inside the exit.** The narrow fix
probably runs the other way from "quiesce devices in recovery" — `dev_shutdown`
does OPEN/CLOSE/EXCP and joins subtasks, exactly the blocking work
`nsfsx_recover_quiesce` refuses to do — and towards *not calling `mm_shutdown()`
in an exit at all*, since §1 already classifies those regions as AS-scoped. That
is #83's decision, not this branch's.

**What it costs: nothing that matters, which is why it is reported and not
patched here.** The pool regions are AS-scoped, so a `free()` that never runs
loses nothing. Critically the `A0A` lands **after** `NSF903I`: the SVC slot is
already restored and the anchor already marked inactive, which is why arm B
restarts cleanly in spite of it. It is a *reporting* defect — recovery ends
without saying it finished, and an operator sees an unexplained second abend
code instead of `NSF901I` — and it means any teardown step added to
`nsf_shutdown()` in future will silently not run in recovery. **Issue #83**,
updated with this arm.

---

## 5. The final regression, and the stand

Final module deployed with **every injection removed** (tree clean, a grep for
the injection markers across `src/` and `include/` returns 0).

| gate | result |
|---|---|
| `S NSFS` / `P NSFS` on the final module | clean both ways — `NSF041I`/`NSF001I`, then `NSF043I SVC 239 RESTORED` → `NSF853I` → `NSF011I` → `IEF404I`, **no `NSF054W`** |
| `TSTRQXC` + `TSTRQXF` | **122 PASS / 0 FAIL, CC 0 batch + TSO** |
| `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH`/`TSTXFW` (NSFV round) | **484 PASS / 0 FAIL, CC 0 batch + TSO** |
| host suite | **2991 PASS / 0 FAIL** — a no-regression check only; both changed files are MVS-only |
| dumps | **none**, whole round (`IEA700I` is the no-dump form) |

`TSTMVCD` excluded, per #53. `NSFV` started and stopped cleanly around its round
(`NSFV034I` → `NSFV095I SVC 239 RESTORED` → `NSFV011I`).

**`NSF043I SVC 239 RESTORED` on the clean stop is the no-regression check that
matters most for the refactor**: the table write now goes through the shared
`nsfsx_svc_restore_locked`, and the clean path is the caller that was already
countersigned.

### Not run: `TSTRQXM`

It needs the CTCI pair, and the pair did not survive the IPLs: Hercules failed
to create the TUN interface at startup (`HHC00138E Error setting TUN/TAP mode :
Interrupted system call` → `HHC01463E 0:0501 device initialization failed`), so
devices 0500/0501 do not exist in the emulator. They can be re-`attach`ed — the
config line verbatim, and `tun0` comes up correctly — but **MVS still answers
`IEE025I UNIT 500 HAS NO LOGICAL PATHS`**: it IPLed without them, and 3.8j has
no dynamic I/O reconfiguration. Only a full Hercules restart cycle recovers it.

`TSTRQXM` exercises the cross-address-space **data** path, which this change
does not touch; what it would add over the 122 + 484 above is coverage of the
transport under load. Recorded as not run, with the reason, rather than quietly
omitted.

### Stand

One `tun0` and one route to `192.168.200.1` (a `devinit` during diagnosis
briefly created a duplicate `tun1` with the same address and a competing route;
`detach` cleared it, and the orphaned `tun0` needs root to remove, so it is left
for the next Hercules restart). Devices 0500/0501 left **detached**. NSFS
running on the final module. CSA largest free block `933888` — one retained
anchor+router from the controlled arm, the designed posture.
