# Issue #64, step 64-0 — the wake measurement

**This is a measurement record, not an ADR and not a decision.** 64-0 fixes nothing by
design: the value of the answer depends entirely on no fix having been applied first.
Whether this becomes ADR-0043 (two defects) or an ADR-0022 annotation plus the fix (one
defect) is not the measurer's call and is deliberately left open here.

Round: MVSCE on `mvsdev`, 2026-08-26, module built from `m5-64-0-wake-probe`.
Console times below are the MVS clock (UTC-5); elapsed figures are wall-clock.

---

## 1. The prediction, as written in the kickoff

> If the cross-AS POST to `g_wake_ecb` had ever landed since `nsfsx_start`, the POSTED bit
> would still be set — nothing clears it — and the WAIT could never block again. The
> executive would spin. It did not: a `F NSFS,STATS` sat unanswered for three minutes. So
> at that moment the ECB was not posted, and since nothing resets it, it had never been
> posted in that instance's life — while `served` stood at `0x18D` = 397 requests.
>
> If that holds, every request that instance ever completed was picked up by the polling
> pre-filter on a pass that happened for some other reason: the transport's wake would not
> have "no floor", it would have no wake.

### The answer: REFUTED — and the kickoff named this branch in advance

> If the measurement shows the POSTED bit set, this model is wrong, the loop cannot have
> been blocking, and the investigation turns into "why is a spinning executive slow".

The POST lands. **`WAKEECB=40000000`, `POSTED=Y`, `WAKEPOSTS` non-zero** the moment the
first cross-address-space request is served. The premise the prediction rested on is
sound — nothing clears the ECB, verified in the wait seam as well as in `nsfsx.c`
(`nsfevt_plat_wait` copies the list into a local array; libc370's `ecb_waitlist` is a bare
`WAIT ECBLIST` and never writes the ECBs) — but the conclusion drawn from the absence of a
spin does not follow, **because the executive does spin**.

---

## 2. A defect 64-0 FOUND — which is not the defect 64-0 was sent to measure

Read this section and §3 together or it will mislead. **The spin below is a newly measured
defect. It is not an explanation of issue #64**, and the evidence that it is not is in §3
and §4. Anyone arriving here on the way to 64-1 should read §4.1 before touching the wake
path.

### The executive spins for the life of the STC after its first request

The latch is permanent and the cost is a full host core. One controlled pair, one STC
instance, the **first request as the only variable**:

| state | POSTED | EVTPASSES | WAKEPOSTS | SERVED | host CPU (user) |
|---|---|---|---|---|---|
| A — fresh, no request yet | **N** | 324 (≈10/s) | 0 | 0 | **0.9 %** |
| B — same STC, after requests | **Y** | 224 817 | 224 330 | 16 | **26.0 %** |

`top -bn2` instantaneous, second sample. Control with NSFS stopped: **0.5–0.7 % user,
99 % idle**, so the 26 % is NSFS and nothing else. (`ps -eo pcpu` is a since-boot average
and was misleading here — it read ~92 % in every state.)

Rates, measured over idle windows with `SERVED` confirmed unchanged across them:

| instance state | EVTPASSES delta | window | rate |
|---|---|---|---|
| fresh, before any request (`POSTED=N`) | 262 → 3 037 | 279 s | **9.95 /s** |
| after requests + TCP workload (`POSTED=Y`) | 1 050 352 → 3 232 684 | 257 s | **8 492 /s** |

9.95/s is the `nsftmr_plat_arm(1u)` heartbeat, blocking correctly on the WAIT between
ticks. 8 492/s is a loop that never blocks.

**`WAKEPOSTS` behaves exactly as its documented semantics predict**, which is what makes
the reading trustworthy rather than merely large: it counts *observations*, not posts, and
nothing clears the ECB, so it latches once and thereafter tracks `EVTPASSES` at a constant
offset. Measured offset on STC 1448: **3 361 at every one of the four readings**
(109 690−106 329, 1 050 352−1 046 991, 3 232 684−3 229 323, 3 390 225−3 386 864) — i.e.
the latch fired at pass 3 361 and every pass since has observed POSTED. On STC 1449 the
offset was 487, and constant likewise.

### The POSTED-bit-only rule earned its keep in this round

On both fresh instances the un-posted ECB read **`009DCD10`** — non-zero, POSTED clear.
That is the RB-address remnant CLAUDE.md §4 names. A non-zero test would have reported
POSTED for an ECB that had never been posted and confirmed the exact opposite of the truth,
in the one place where it would have been believed.

---

## 3. The stall did NOT reproduce, in either arm

Both arms are reported as measured; neither is stretched into a conclusion.

| STC | prior workload | idle before | 8 sequential requests (SOLO) | EVTPASSES before → after | WAKEPOSTS | wake-ECB word | TMRQ | swap | latency |
|---|---|---|---|---|---|---|---|---|---|
| 1448 | none (fresh) | 279 s | 07.14.14 → 07.14.15 | 3 037 → 109 690 | 0 → 106 329 | `009DCD10` → `40000000` | 0 | `V=V`, non-swappable | **< 1 s** |
| 1448 | `TSTRQXM` (TCP: sockets, connections, 9 353 bytes short-write) | 257 s | 07.20.50 → 07.20.51 | 3 232 684 → 3 390 225 | 3 229 323 → 3 386 864 | `40000000` | 0 | `V=V`, non-swappable | **< 1 s** |
| 1449 | none (fresh) | 30 s | one run, CC 0 | 324 → 224 817 | 0 → 224 330 | `009DCD10` → `40000000` | 0 | `V=V`, non-swappable | **< 1 s** |

Both `TSTRQXC` SOLO runs completed all 8 requests inside a single console-timestamp second,
batch and TSO, CC 0, 16 PASS. Against issue #64's slow instance at *~3 requests per minute*
with one request taking 11 minutes, this round is 2–3 orders of magnitude away from the
defect. **A run in which the stall fails to reproduce is not evidence that anything works.**

`D A,NSFS` and `D J,NSFS` are `IEE535I INVALID PARAMETER` on 3.8j; `D A,L` shows NSFS as
`V=V` with no `S` flag (TSO and UFSD carry it), so **the address space is non-swappable and
swap-out is excluded** as a mechanism.

---

## 4. What was not measured, and what stays open

- **`g_armed` has no accessor and 64-0 did not add one.** `nsftmr_count()` (reported as
  `TMRQ`) read 0 in every single reading, which does **not** imply the STIMER is disarmed:
  `nsfsmain`'s `nsftmr_plat_arm(1u)` heartbeat sits deliberately outside `g_armed`'s
  bookkeeping. The substitute is not a limitation but a genuine measurement — EVTPASSES
  growth over a known-idle window, which is what the 9.95/s vs 8 492/s rows above are.
- **The 11-minute stall itself was not reproduced**, so its mechanism is not established
  and is not claimed here.
### 4.1 What this means for 64-1 — the part that changes what it does

Resetting `g_wake_ecb` in `nsfsx_drain` (Phase 1's `nsfreq_drain` contract, issue #64's
asymmetry 1) will remove the spin. It is very likely the right change on its own merits.
**But there is no evidence here that it addresses the stall, and one reason to expect it
will not:** with the reset in place and `TMRQ=0`, the loop returns to blocking on the WAIT
with only the `nsftmr_plat_arm(1u)` heartbeat underneath it — which is precisely the
requestless configuration measured as state A above. On this stand state A is fast. It is
also exactly the configuration issue #64 describes as slow. So 64-1 should expect to fix
the spin and then still have to reproduce #64 to know whether anything was gained.

### 4.2 Candidates

- **The fresh-vs-stale asymmetry is reported, not resolved.** This round's own asymmetry
  runs on a different axis than issue #64's (first-request-yet vs prior-TCP-workload), and
  attributing #64's to whichever candidate a fix would address is exactly the move the
  kickoff forbids. One observation constrains it, offered as such and not as a diagnosis:
  #64's slow instance stood at `served = 0x18D` = 397, so on the code as it stands its wake
  ECB should have latched long before and it should have been **spinning, not blocking**.
  Something in that triangle is unaccounted for.
- **The POST-target fallback is EXCLUDED on this stand** — not by a further test but by the
  reading already taken. `asm/nsfvsvc.asm` posts the STC-private key-8 ECB when `ANCSEPTR`
  is published and otherwise falls back to the key-0 CSA `server_ecb`, which is **not** in
  the executive's ECBLIST and would never wake it:

  ```
  L     R11,ANCSEPTR(,R2)   LTR R11,R11   BNZ PSTECBX   LA R11,ANCSECB(,R2)
  ```

  `NSF812I` reports `g_wake_ecb` — *the private ECB itself* — and it read `40000000`. A
  POST completion code can only be in that word if the `BNZ` branch was taken. So the
  publication is correct here and the fallback did not run. What that does **not** settle
  is whether it ran on #64's instance, which was not observed.
- **Still untested:** whatever a two-client gate leaves behind that a single client does
  not. #64's slow instance had run both `TSTRQXM` and a full two-client gate; this round
  ran `TSTRQXM` only. That is the most obvious untried arm and it is the one to try next.
- **The `nsfsx_drain` reset asymmetry against Phase 1's `nsfreq_drain` is confirmed as
  real** (`g_wake_ecb` is assigned once, in `nsfsx_start`), and it is now measured to cause
  a permanent spin. Whether it also causes the stall is *not* shown.

## 5. Round hygiene

`NSF810I` read **52 COUNTER(S)** against 50 on the pre-change instances (STC 1431, 1445) and
NSF812I/NSF813I were present — the deploy-took-effect check, passed before any number below
it was believed. Deploy order was `P NSFS` → `make deploy` → `S NSFS`; no mid-chain HTTP 500.
Both instances reported `NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144) -- LARGEST FREE
BLOCK NOW 933888` — identical, so the round leaked no CSA. Every stop was clean: `NSF043I
SVC 239 RESTORED`, `NSF044I`, `NSF011I`, **no `NSF054W` retain, no dump**. NSFS is left
**stopped** rather than spinning at a core; `S NSFS` restores it. **TESTLIB holds
`TSTRQXC` alone** — the last `--only` run replaced it — so the next round must re-deploy
whatever it needs rather than assume the Stage-0 set is present (the b4 `S806`).

### A pre-existing reporting defect, found and not fixed

**`F NSFS,STATS` silently truncates.** `NSF810I` reports 52 counters; only **32–33**
`NSF811I` lines are rendered, and the cut point moves as counter values gain digits (33 in
one reading, 32 in the next). `sts_render`'s 512-byte buffer stops at the last whole line
that fits, so ~20 counters are dropped with nothing saying so. It is out of scope for 64-0
and is reported rather than fixed — but it is why NSF812I/NSF813I are WTOed *after* the
render instead of being two more counters at the end of the registry, where they would have
been invisible exactly when needed.
