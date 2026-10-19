# 64-0f round log — the spin arm

**Clock:** MVS local = host CEST − 7 h exactly. Measured, not assumed: STC01491
started at MVS `12.23.07` / host `19:23:07 CEST`. Never string-compare these
(64-0e's `awk '$2 >= "9.36"'` dropped everything after 10:00 and reported a
clean null).

**Every intervention is logged, including anything that looks passive.**

| host time | MVS local | what | why / result |
|---|---|---|---|
| 18:26 | 11:26 | `D T`, `D A,L` | stand alive; **no NSFS/NSFV running**; HTTPD up (needed for `/.dm`) |
| 18:5x | 11:5x | **job CBOFF7 submitted** | derive `ASCBSTOR` from `SYS1.AMODGEN`; it was NOT in CBOFF5/CBOFF6 and it is this round's instrument |
| 19:22:08 | 12:22:08 | **INTERVENTION — purged 27 jobs** | 26 prior-round `NSFS`/`NSFV`/`NSF` STCs in OUTPUT + `CBOFF7`. Spool had hit `$HASP355` ~1000 console lines earlier (64-0e's incident). Maintainer chose this option. |
| 19:22:5x | 12:22:5x | `make modules` + `make deploy` | the **revert** build. Deploy clean — no mid-chain `HTTP 500` on `DELETE /restfiles/ds/NSF.LINKLIB` |
| **19:23:06** | **12:23:06** | **`S NSFS` — DEFECTIVE BUILD LIVE** | STC01491, anchor `00A8B7C8`, `NSF210I CTCI 0500/0501 UP ... MTU 1500`, `NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144)` |
| 19:26:00 | 12:26:00 | swapwatch read-path validation | OUCB identity proof PASSES; `ASCBSTOR` reads |
| 19:27:55 | 12:27:55 | swapwatch armed (background) | ASCB `FF8B20`, ASID `000B`, fast 5 s / full 45 s |
| 19:28–19:29 | 12:28–12:29 | **segment A** — `TSTRQXM` | batch **CC 0**; TSO CC 1 **by design** (one-shot listener consumed by the batch run — TSTTCPW precedent) |
| 19:29:07 | 12:29:07 | **`F NSFS,STATS` — deploy check** | `POSTED=Y ... SERVED=33` ✅ and `TMRQ=0` (floor gone) |
| 19:30:32 | 12:30:32 | **B1-W1 start** (idle) | nothing touching the stack |
| 19:37:12 | 12:37:12 | B1-W1 end | 400 s, `served` 33 → **33 frozen** |
| 19:37:43 | 12:37:43 | B1-W2 start | |
| 19:44:23 | 12:44:23 | B1-W2 end | 400 s, `served` 33 → **33 frozen**. B1 total **800 s** |
| 19:39 | 12:39 | host CPU delta sample | **112.7 %** of a host core (`/proc/<pid>/stat` delta, never `ps pcpu`) |
| 19:45 | 12:45 | `make test-mvs --only TSTRQXC` | deploy TSTRQXC — the `--only TSTRQXM` run had displaced it from TESTLIB (the b4 S806 trap) |
| **19:44:30–19:45:16** | **12:44:30–12:45:16** | **STALL ONSET (B2, F(i))** | last healthy full sample `QFL=00 CPUS=1` EJST moving; first stalled `QFL=80[GOO] SRC=09 CPUS=0` EJST frozen. Load (`TSTRQXC`) began ~19:44:30 → onset **inside the documented 40–90 s band** |
| 19:47:26 | 12:47:26 | anchor read (no MODIFY) | `served=33` frozen, `inflight=1`, census **`PENDING: 1`, FREE 63** |
| 19:48:21 | 12:48:21 | full `asread.py` reading | `OUCBASCB=FF8B20 IDENTITY OK`; `QFL=80[GOO] SRC=09`, `ASCBSTOR=0FAF3C00` unchanged, `CPUS=0`, `OUXBRSW=00000000`, `NDS=0` |
| 19:49:40 | 12:49:40 | **stall detector armed** | fired at 19:50:11: *"EJST flat, served=33 frozen, 1 slot(s) PENDING for 22s"*, `SLOT0 PENDING reply_ecb=809DE5F0 ascb=FD0F18 asid=0008 xfunc=6`; still stalled at 91 s |
| 19:52:14 | 12:52:14 | duration watcher armed | to record whether the stall self-clears |
| 19:53 | 12:53 | **correction applied to the doc** | the 12 `B2LOAD` sustained-load jobs had **not** been submitted when the stall fired — what ran was a `make test-mvs` deploy burst. Band-membership claim withdrawn; §B2b added to run the intended comparison |
| **19:57:15** | **12:57:15** | **STALL CLEARED BY ITSELF** | `ASCBSTOR 0FAF3C00 → 0FC26C00` (**a swap cycle COMPLETED**), `OUCBSWC 0 → 1`, `QFL 80[GOO] → 00[-]`, `served 33 → 49`. **No intervention was made** — stopping the STC would have destroyed this reading. Duration **~12.0–12.7 min** |
| 19:57:38 | 12:57:38 | `JOB02610` re-checked | **`CC 0000`** — the client was parked, never failing |
| 19:58:34 | 12:58:34 | **B2b armed + started** | the sustained-load arm actually intended: detector + 12 × 400-step `B2LOAD` jobs, no deploy activity |
| 20:01:12 | 13:01:12 | **B2b first attempt FAILED — caught, not assumed** | `served` frozen at 49 with all 64 slots FREE ⇒ no client was submitting. All 12 jobs `JCL ERROR`: **`IEF602I EXCESSIVE NUMBER OF EXECUTE STATEMENTS`** — MVS 3.8j caps a job at **255** EXEC steps and they had 400. Had `served` not been checked, this would have been reported as a quiet load arm |
| 20:02:1x | 13:02:1x | 12 failed jobs purged; jobs regenerated at 250 steps | |
| 20:02:26 | 13:02:26 | **one job submitted first, and verified** | `served` **49 → 753 in 25 s** (~28 req/s) before submitting the rest — a positive check that the load lands |
| 20:03:00 | 13:03:00 | 13 further jobs submitted | sustained load, no deploy activity |
| 20:13:16 | 13:13:16 | **B2b result: NO STALL** | `served` **49 → 28 049** = **28 000 requests** in ~15 min at up to ~102 req/s, all 14 jobs `OUTPUT`. `QFL=00` throughout. The stall in §B2 fired at `served=33` |
| 20:13:45 | 13:13:45 | **B2c armed + started** | repeat of the EXACT condition that provoked the stall — the same `make test-mvs --only TSTRQXC` deploy cycle |
| 20:14:09 | 13:14:09 | **B2c result: NO STALL** | the identical `make test-mvs --only TSTRQXC` cycle ran clean — `TSTRQXC ok CC 0` batch **and** TSO, 16 PASS. The stall is **not reproducible on demand**: one onset in three deliberate attempts |
| 20:14 | 13:14 | **arm 2 decision** | specified §3.5 arm deliberately **not run** (maintainer's call): its purpose — separating spin from read — is unattainable when the phenomenon fires ~1 in 3, and it is confounded (device offline also removes the read subtasks and 2 TCBs) |
| 20:23:27 | 13:23:27 | **B2d idle phase start** | testing the one candidate condition: ~840 s idle then a deploy burst, matched to the only stall's actual sequence (833 s idle → burst). `served=28065` frozen |
| 20:37:30 | 13:37:30 | **B2d burst (after 840 s idle)** | **NO STALL** — `TSTRQXC ok CC 0` batch **and** TSO, 16 PASS. The candidate condition is refuted too: **one onset in four deliberate attempts** |
| **20:37:55** | **13:37:55** | **`P NSFS` — defective build STOPPED** | `NSF011I NSFS SHUTDOWN COMPLETE`, `$HASP395 NSFS ENDED`. **Wall clock live: 19:23:06 → 20:37:55 = 1 h 14 min 49 s** |
| 20:38:0x | 13:38:0x | `src/nsfsx.c` restored to `main` | `git diff --stat` **empty** — verified, not assumed |
| 20:38:2x | 13:38:2x | `make modules` + `make deploy` | clean, no mid-chain `HTTP 500` |
| 20:38:28 | 13:38:28 | `S NSFS` on the restored module | anchor `00AAF7C8` (new), `NSF001I` |
| 20:38:5x | 13:38:5x | workload on the restored module | `TSTRQXC` **ok CC 0 batch AND TSO**, 16 PASS |
| **20:39:01** | **13:39:01** | **RESTORE CHECK** | `NSF812I WAKEECB=00000000 POSTED=N EVTPASSES=345 WAKEPOSTS=16 WPREG=Y SERVED=16` — reset build live. `WAKEPOSTS == SERVED` and `EVTPASSES` 345 (vs 529 481 spinning) corroborate independently |
| 20:39:4x | 13:39:4x | samplers stopped | round complete; stand on `main`'s module |

## Spool

Purged 27 jobs at 19:22:08 (maintainer's choice) plus 12 failed `B2LOAD` jobs at
20:02. **No `$HASP355` occurred during the round** — the count in `mvslog.txt`
stayed at 23 throughout, checked at the start and under load.
