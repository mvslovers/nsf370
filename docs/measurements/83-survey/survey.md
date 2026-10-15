# #83 survey: the A0A inside recovery

Read from source on 2026-10-15. **It decides nothing**: it names the fix
options and leaves the choice to the maintainer. Nothing here has been run;
every claim marked *inference* is one.

## 1. What `A0A- 2` means

`IEA700I A0A-  2 NSFS NSFS 00 0000A040 0015DFC0 009DC7B0`, byte-identical in
all five #79 occurrences (`docs/measurements/m5-79/findings.md` §4).

**MVS 3.8 IEAVGM00** (the GETMAIN/FREEMAIN module, mvs38src
`work/measurements/amvssrc-tk5-run8/IEAVGM00`), in the abend table of the
prologue:

```
A05, A0A, A78     1      4    AREA BEING FREED OVERLAPS A FREE AREA
                  2      8 #  PART OF AREA BEING FREED IS STILL FIXED
#  FOR A CONDITIONAL REQUEST WITH "TCBEOTFM" BIT IN TCB ... SET TO 1,
   THIS ERROR WILL GIVE A RETURN CODE OF 8.  OTHERWISE IT WILL BE ABENDED.
```

**`A0A` reason 2 is a FREEMAIN of storage that RSM still holds fixed.** It is
not a corrupt free chain and not a heap race. `RQSTSIZE`, commented "SAVED SIZE
... FOR MSG IEA700I ON AXX-2 ABENDS", fills the length field. So the operands
read as follows:
- subpool 0, length X'A040';
- the address being freed, `0015DFC0`;
- the requesting TCB, `009DC7B0`.

This decoding comes from IEAVGM00's message setup; a subagent read it out of
the source. The table row itself is quoted above.

## 2. Which block it is

libc370 has no heap of its own. `malloc` is one `GETMAIN` per call, rounding
size+8 up to a multiple of 64, and `free` is one unconditional
`FREEMAIN R` (SVC 10) with no check of the return code
(`libc370 src/mvs/storage/@@getm.asm:31-32`, `@@freem.asm:20-26`).

The NSFMM region of the CTCI buffer pool is `2 × (8 + CTCI_BUF_DEFAULT)`:
- `CTCI_BUF_DEFAULT` is `0x5000` (`include/nsfctci.h:83`);
- 8 bytes are `sizeof(MMOBJ)`, the pool stride overhead (`src/nsfmm.c:101`);
- so the region is 40976 bytes = X'A010'. libc370 adds 8 to that and rounds
  up to 64, which gives **X'A040' = 41024**.

**The freed block is very likely the `CTCIBUF` pool region**
(`src/nsfctcib.c:106`). BUFSMALL (64 × 296) and BUFLARGE (128 × 2088) are far
from that size. The socket, TCP, UDP, event, CTCIDEV and CTCISCB pools were
**not** sized here. So this is an inference from the arithmetic, and the
address was not compared live.

## 3. Why it is still fixed

The CTCI READ subtask parks an EXCP READ on the CTCI device until traffic
arrives (ADR-0022, ADR-0027). EXCP on a V=V region page-fixes the data areas
of the channel program for the duration of the I/O, and the READ buffer is a
`CTCIBUF` object. While the READ is outstanding, part of that region is
fixed, and `mm_shutdown()` → `free()` → `FREEMAIN` abends `A0A-2`.

*Inference:* that EXCP fixes the buffer pages is MVS behaviour that is not
re-read here. Everything measured fits it:

| Observation (#79, 40-CHK) | Fits because |
|---|---|
| CTCI up → `A0A`, three times; CTCI down → `NSF901I`, once, same binary | no device, no EXCP, nothing fixed |
| `A0A` also on the ordinary startup-failure path, **with no ESTAE at all** (40-CHK STC 1506: `NSF049E` → `NSF009E` → `nsf_shutdown()` → `return 8` → `SA0A`) | the defect is "free under outstanding I/O", not "free in an exit" |
| 40-CHK STC 1505 reached `NSF901I` with CTCI up | it abended on the RECVFROM completion path: a READ had just completed and was perhaps not yet re-armed (*inference*, not checked) |
| Operands identical every time | the same region, the same length, the same main TCB |
| The clean stop never `A0A`s | `dev_foreach(nsf_quiesce_device)` runs first; the subtasks CLOSE (purging their EXCP) and are joined before `nsf_shutdown()` (`src/nsfsmain.c:521`, `src/nsfctcib.c:676-684`) |

The "libc370 heap is not reentrant across subtasks" reading in the #83 comment
is **superseded**: there is no shared free list to race on.

## 4. The paths that free under outstanding I/O

| # | Path | Devices live? | Today |
|---|---|---|---|
| P1 | `nsf_recover` in `src/nsfsmain.c:158` (NSFS) | yes, after `S NSFS` came up | `A0A`; `NSF901I` never shown |
| P2 | `nsf_recover` in `src/nsfmain.c:63` (Phase 1, NSF) | yes | same code, so presumably the same; never measured |
| P3 | `nsfsx_start` failure, `src/nsfsmain.c:444` (`nsf_start_devices` ran at :416) | yes | `SA0A` instead of a clean RC 8 (40-CHK STC 1506) |
| P4 | clean stop when a subtask will not join: `ctci_op_shutdown` says `NSF206W ... STORAGE RETAINED` and keeps the device, then `nsf_shutdown()` frees the region anyway | yes, by definition | latent: the "retained" storage is freed one call later; never observed |

The other `nsf_shutdown()` and `mm_shutdown()` calls on the startup paths run
before `nsf_start_devices`: `src/nsfsmain.c:406` and the NSF009E reserve path,
and `src/nsfmain.c:257-279`.

## 5. Fix options

**A. Recovery does not free the regions.** `nsf_recover` calls a recovery
teardown that disarms the STIMER exit and stops there; no `mm_shutdown()`.
- The regions are private-area storage, which MVS reclaims at address-space
  end. #79's audit already sorts them into that group
  (`docs/measurements/m5-79/audit.md` §1).
- At task end MVS purges the outstanding I/O before it frees the region
  (*inference*, standard end-of-task processing, not re-read here). The `#`
  row above shows that VSM expects fixed storage at end of task, and answers
  it with RC 8 under `TCBEOTFM` rather than an abend.
- Fixes P1 and P2. It is the line #79 drew, applied one step further: recovery
  releases only what nobody else will.

**B. Recovery halts the I/O first** (IOHALT/PURGE the parked READ, join the
subtasks, then free). Rejected as the recovery fix: it is device work and a
bounded wait inside an exit in a damaged environment. That is exactly what
`nsfsx_recover_quiesce` refuses to do, for the same reason.

**C. The mainline failure path quiesces the devices first.** At
`src/nsfsmain.c:444`, `dev_foreach(nsf_quiesce_device, NULL)` before
`nsf_shutdown()`, as the clean stop does. Fixes P3. It is mainline code, so
the blocking join is allowed.

**D. `mm_shutdown()` refuses to free while a device may have I/O outstanding.**
- The device layer reports whether any device is still started or retained.
- `nsf_shutdown()` then skips `mm_shutdown()` and says so (a third state,
  §8.5: "not freed, and why").
- Fixes P4, and makes P1–P3 safe even if a later caller forgets C.
- Costs one new query on NSFDEV and one message.

**Recommendation: A + C, with D as the backstop.** A and C are each a few
lines on a path that is understood. D is what keeps a fifth path from
reappearing as the same `A0A`.

## 6. How the round would test it

**Host.** The decisions can be pinned, the fixed page cannot:
- recovery teardown leaves `mm_debug_live_regions()` unchanged;
- with a device marked live, `nsf_shutdown()` skips the free and reports it.

A green host run says nothing about RSM.

**Live, red first, cheapest first:**
1. **P3, no injection needed.** `S NSFV` takes SVC 239. Then `S NSFS` with
   CTCI up: today `NSF049E` → `NSF009E` → `SA0A`, the 40-CHK STC 1506 shape.
   After C: RC 8 and no abend. The prediction includes the `IEA700I` operands
   `00 0000A040`.
   - Anchor and router are freed on that path (`src/nsfsx.c:953-956`), so it
     leaves no CSA behind.
   - NSFV has to come down afterwards.
2. **P1, needs an abend on the executive.**
   - #79 used a temporary, never-committed `S0C4` injection reached by
     `F NSFS,STATS`.
   - Today: `NSF903I` then `A0A`. After A: `NSF903I`, then **`NSF901I`**, then
     the original `S0C4` percolates.
   - The anchor is then retained by recovery. The next `S NSFS` should adopt
     it, which would be **ADR-0048's ESTAE retain path live** — the row d2
     could not run.
   - IPL risk is lower than in #79, because the SVC slot is now restored
     before anything that can fault. It is not nil.
3. **P2 and P4** stay host-pinned unless a cheap provocation turns up.

## 7. Decisions for the maintainer

1. Fix shape: **A + C (+ D)**, or a subset.
2. The abend injection for step 2: a temporary uncommitted patch, as in #79,
   or a committed test hook? A committed hook is a production abend verb and
   would need its own guard.
3. Whether step 2 also carries the d2 ESTAE retain/adopt rows, or that is a
   round of its own.

## Not established

- That the fixed pages are the READ buffer: the region is identified by
  length only, and no address or TCB was compared live.
- Whether `0015DFC0` is the region base minus 8, and whether `009DC7B0` is the
  executive's TCB. A live step 1 can print both.
- P2 and P4 have never been observed.
