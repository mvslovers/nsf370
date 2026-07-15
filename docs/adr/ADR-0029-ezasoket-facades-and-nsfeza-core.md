# ADR-0029 — Socket API layering: thin facades over one surface-neutral NSFEZA core

**Status:** Accepted (2026-07-15); amended same day after extraction of
SC31-7187-03 (Appendix D + §5.4) into `docs/ezasoket-conformance.md`.
Introduced with M3-4 (NSFEZA). **Relates to:**
§15 (EZASOKET API), §15.2 (M6 completeness audit), ADR-0028 (M3-3 seams),
the frozen NSFRQE contract (§14, frozen at M3), RQ_INITAPI registration (M3-2).

M3-4 is where NSF grows an application-visible API, and the shape chosen now is
the shape every later caller links against. This ADR fixes the layering before
the first line of NSFEZA is written, because the alternative — one facade with
the core logic baked into it — is cheap now and expensive to unbake.

## Context: there are TWO compatibility surfaces, not one

Research against the live ecosystem (repos pinned below) established:

1. **EZASOKET / EZASMI — the IBM surface §15 prescribes.** Two distinct IBM
   interfaces (WAVV 2006, IBM's own architecture chart): the **EZASMI macro**
   interface for HLASM, and the **`CALL 'EZASOKET'`** HLL call interface
   (CL16 blank-padded function name, ERRNO/RETCODE always the last two
   parameters, halfword socket descriptor, SOCKET returns the descriptor in
   RETCODE — GG24-2561-00). INITAPI/TERMAPI are macro-side; the HLL call
   interface performs them implicitly.

2. **libc370 `socket.h` → `@@75xxxx` — what today's applications actually
   call.** HTTPD calls `socket()/accept()/select()/getsockname()`, mvsMF calls
   `recv()` (28 uses). The header aliases (`asm("@@75SOCK")` etc.) resolve to
   `libc370/src/dyn75/`, which builds the X'75' parameter list (`pl75`,
   `include/__75.h`) for the Hercules instruction hack. **"Relink-only" for
   HTTPD/mvsMF therefore does NOT route through EZASOKET.** §15.2 defers the
   `@@75` audit to M6, but the layering must accommodate it now.

Additionally, Shelby Beach's EZASMI (MVS-sysgen/SOFTWARE, `EZASMI/`) pins a
third fact: the macro emits **no X'75'**. It builds an address list and calls a
single entry point, **EZASOH03** (`R1` → `+0` A(4-char EBCDIC function code
`SOCK`/`BIND`/`CLOS`/…), `+4` A(ERRNO), `+8` A(RETCODE), `+12…`
function-specific; `R15` always 0 — real errors live in RETCODE/ERRNO).
`SOURCE/EZASOH03.txt` is the X'75' backend behind it. **Swapping the backend =
replacing EZASOH03; the macro set is untouched.**

## Decision

**Thin facades over one surface-neutral core.** The core, NSFEZA, is exactly
what §15.1 already describes: build an NSFRQE, submit, WAIT, map completion to
RETCODE/ERRNO. No facade contains socket logic; each only marshals its calling
convention into the core.

```
EZASMI macros (Shelby's, unmodified) ──► EZASOH03 facade (NSF-provided) ─┐
                                                                          ├─► NSFEZA core:
C API (new, own alias namespace, e.g. @@NSSOCK) ─────────────────────────┘    build RQE →
                                                                              submit → WAIT →
CALL 'EZASOKET' CL16 facade (later, when a caller exists)                     map RETCODE/ERRNO
                                                                                    │
                                                                                    ▼
                                                                          NSFRQE (frozen, M3-2)
```

M3-4 ships two facades: the **EZASOH03 replacement** (so the existing EZASMI
macro ecosystem runs on NSF by swapping one module) and the **C API** in its
own `asm()` alias namespace (`@@NS…`), deliberately disjoint from `@@75…` so it
links **alongside** dyn75 with no collision. The CL16 `EZASOKET` entry point is
a third thin facade over the same core, added when a caller needs it (M6 audit
decides); nothing in the core assumes it doesn't exist.

### Facade contract: union of functions, graceful subsets

Shelby's `TYPE=` list is TCP-only — **no SENDTO/RECVFROM** (verified: absent
from `MACLIB/EZASMI.hlasm`); libc370's `socket.h` lacks them too. UDP is a
genuinely new capability of NSF. The macro/function set is the **union**;
each backend serves a subset; an unsupported function returns cleanly with
`RETCODE=-1, ERRNO=EOPNOTSUPP` and `R15=0` — never an abend, matching the
pinned EZASOH03 ABI (R15 always 0).

M3-4 function set (§15.2): INITAPI, SOCKET, BIND, SENDTO, RECVFROM, CLOSE,
TERMAPI, GETSOCKNAME.

### Descriptor model: halfword API numbers mapped onto generation descriptors

SC31-7187-03 (§5.4.16/§5.4.31, verified) fixes the API-visible socket
descriptor as a **halfword** (`pic 9(4) Binary`), **numbered from 0**
("If you have 50 sockets, they are numbered from 0 to 49"); MAXSOC is
**min 50 / max 2000, defaulting to 50** if less is requested, and MAXSNO
returns the highest assignable number. (Shelby's MVS 3.8 port uses 1–1023 —
an ecosystem deviation, recorded in the conformance doc.) Our internal
descriptor is a fullword `(gen<<16)|id` with a generation counter — it
cannot be surfaced. Therefore NSFEZA keeps a **per-application mapping
table** (socket number → internal descriptor), anchored at the INITAPI
registration (RQ_INITAPI, M3-2):

- Socket numbers are **0-based** per IBM; the mapping table is sized by the
  clamped MAXSOC.
- CLOSE clears the entry → any later use of that number draws **EBADF (9)**
  at the facade; the gen-check in the core still catches internal reuse.
  Stale-handle protection is preserved end to end.
- `RQ_SOCKET` **auto-registers** the application when no INITAPI preceded it
  (implicit INITAPI — IBM behavior; SC31-7187 lists GETCLIENTID, GETHOSTID,
  GETHOSTNAME, GETIBMOPT, SELECT, SELECTEX, SOCKET, TAKESOCKET as implicit
  triggers; Shelby implements SOCKET, SELECT, GETHOSTBY*).
- MAXSOC is accepted and **clamped** against the pool limit (SOCKET pool
  default 64); MAXSNO reports the clamped reality, no over-promising.
- **TERMAPI takes no ERRNO/RETCODE** (`CALL 'EZASOKET' USING SOC-FUNCTION.`
  only) — the "ERRNO/RETCODE last" rule holds for every other M3-4 function.

### ERRNO policy — the suspected ecosystem divergence is RESOLVED

§15 mandates **IBM EZASOKET ERRNO values**. The original suspicion — that
`EZASOH03.txt` line 314 (`LA R4,61 … hECONNREFUSED`, BSD value 61) meant the
X'75' ecosystem diverges from IBM — **dissolved on extraction of
SC31-7187-03 Appendix D**: IBM Table 67 itself lists `ECONNREFUSED = 61`.
IBM's V3R2 socket error numbers ARE the BSD-derived set (EBADF=9, EMFILE=24,
EWOULDBLOCK=35, EOPNOTSUPP=45, EADDRINUSE=48, ECONNRESET=54,
ECONNREFUSED=61, …). §15, Shelby's backend, and the `@@75` ecosystem agree;
there is no relink-compatibility conflict.

- NSFEZA core maps to the **classic Table 67 values**. Unsupported functions
  return RETCODE=-1, ERRNO=45 (EOPNOTSUPP), R15=0.
- Table 67 also carries **OE aliases** (EBADF=113, EINVAL=121, E2BIG=145
  beside 9/22/7) — OpenEdition errno values merged into the same table. NSF
  returns the classic (low) values; the aliases are recorded in the
  conformance doc for the M6 audit only.
- The full Table 67 (224 rows) and Table 68 (sockets extended, 10xxx,
  137 rows) live in **`docs/ezasoket-conformance.md`** (created with this
  amendment; the M6 acceptance artifact per §15.2).

### dyn75 succession plan (direction, not M3-4 scope)

The new C API lives beside dyn75 (disjoint alias namespace, no link
collision). Once stable and proven on MVS, dyn75 moves into a
subdirectory/subpackage of libc370 and the canonical BSD names
(`socket()` etc.) re-point to the NSF backend — that is the relink-only story
for HTTPD/mvsMF, and it is an M6 deliverable gated on the audit.

## Why facades-over-core, not one monolithic API module

- **§15.1 already specifies the core** ("translate each call into an NSFRQE,
  submit it, WAIT, and map completion back"). Making that a shared component
  is zero extra machinery; duplicating it per facade is the expensive path.
- **The EZASOH03 seam is free.** Shelby's design routes ALL macro traffic
  through one entry point whose header says "The EZASMI macro provides the
  interface to this module." Replacing one module gives NSF the entire
  existing EZASMI ecosystem without touching a macro. Refusing that seam and
  re-implementing the macro side would be hand-rolling a convention the
  ecosystem already settled — exactly the failure mode this project's every
  hard bug has traced back to.
- **IBM's own architecture validates it** (WAVV 2006): BSD/C calls are the
  base layer; EZASMI, EZASOKET, and REXX are facades offering subsets. We are
  reproducing the vendor's proven shape, not inventing one.
- **The frozen NSFRQE contract stays the single choke point.** Every facade
  funnels into the same RQE build/submit/WAIT/map path — one place to test,
  one place for the stale-descriptor check, one ERRNO mapping table.

## Consequences

- NSF owns entry point name **EZASOH03** (and later **EZASOKET**); deployment
  must ensure link order picks NSF's module over Shelby's original when both
  are installed. Documented in the conformance doc.
- The per-app socket-number table is a new fixed-size structure sized by
  clamped MAXSOC (≤ pool limit 64 in v1); exhaustion returns EMFILE-class
  ERRNO, logged, never silently reused.
- Shelby's EZASMI is synchronous-only (no ECB/ERROR/NS/REQAREA on SOCKET);
  the EZASOH03 facade may therefore be synchronous in v1. Async macro forms,
  if ever supported, are a facade-level feature — the core's
  submit→WAIT seam already isolates it.
- IBM's "Sockets Extended call interface does not support MVS multitasking"
  restriction applies to the CL16 EZASOKET facade only; the macro facade and
  C API follow Shelby's model (caller-cleared task storage before INITAPI,
  `MF=` for multiple tasks per address space).
- #28 (IOHALT against a subchannel with no outstanding I/O) becomes reachable
  once M3-4 exposes locally-originated sends to applications — it must be
  resolved or explicitly fenced in the M3-4 PR, not carried silently.

## Evidence pins

- `SOFTWARE/EZASMI/MACLIB/EZASMI.hlasm` — macro → EZASOH03, 4-char `&FUNC`
  codes, TCP-only TYPE list (no SENDTO/RECVFROM).
- `SOFTWARE/EZASMI/SOURCE/EZASOH03.txt` — X'75' backend
  (`DC X'75005000'`, line 1139); `LA R4,61` hECONNREFUSED (line 314).
- `libc370/include/socket.h`, `include/__75.h`, `src/dyn75/` — the `@@75`
  BSD surface HTTPD/mvsMF link against today.
- GG24-2561-00 — CL16 SOC-FUNCTION, halfword descriptor, ERRNO/RETCODE last,
  SOCKET result in RETCODE, INITAPI/SOCKET/BIND/SENDTO/RECVFROM parameter
  lists.
- WAVV 2006 (dinomasters.com/coolstuff/2006EZA.pdf) — facade architecture,
  EZASMI ≠ EZASOKET, INITAPI/TERMAPI macro-only.
- SC31-7187-03 Appendix D + §5.4 — extracted into
  `docs/ezasoket-conformance.md`: Table 67 confirms ECONNREFUSED=61
  (BSD-derived set); all eight M3-4 call formats verified; extended code
  10110 "LOAD of EZASOH03 failed" pins **EZASOH03 as IBM's own module
  name** — the facade replaces the IBM-designated seam.

## Amendment (M3-4 implementation)

Implementation forced three deltas, recorded here (not silently):

1. **Veneer form: PDPPRLG, not FUNHEAD.** The decision above ("thin facades
   over one core; the facade marshals to the C core") left the EZASOH03
   entry's prologue open. It must CALL a cc370 C function
   (`nsf_ezasoh03`/`@@NSOH03`), and the cc370 C prologue
   (`maclib/pdpprlg.macro`) allocates the callee's DSA from the caller's
   **DSANAB at `76(R13)`** — so the entry must present a valid cc370 DSA.
   `FUNHEAD` never sets DSANAB; a C call after it reads a garbage NAB and
   corrupts the save chain (the issue-#8 S0C6-on-the-next-call class). The
   PROVEN pattern in this ecosystem is `PDPPRLG`: libc370's VSAM exit stubs
   (`src/clib/@@vsopen.c` — the `EODAD`/`LERAD`/`SYNAD` routines written in
   file-scope `__asm__`) are EZASOH03's analog — HAND-WRITTEN asm, entered by
   a non-C caller (the VSAM access method, as EZASOH03 is entered by the
   EZASMI macro), calling C via `PDPPRLG` + `L R15,=V(@@VSXEOF)` + `BALR`.
   NSF's veneer follows them. (Verified: `pdpprlg.macro` reads/stores the NAB
   at `76(,13)`; `mvsmacs.macro`'s `FUNHEAD` never references offset 76.)
   `PDPPRLG` also yields a
   per-invocation DSA off each caller's C stack — concurrency-safe for the
   app subtasks that enter EZASOH03, with no static save area and no
   GETMAIN. (The M3-4 caller is always in C context — a relinked C app, or
   the cthread test drivers; a pure-asm EZASMI caller with no C environment
   is an M6 relink-audit concern and hooks in at the veneer without touching
   the decoder.)
2. **New UDP codes SNDT / RCVF** (conformance doc §2.1): Shelby's
   first-4-char `&FUNC` scheme collides SENDTO→SEND, RECVFROM→RECV, so NSF
   pins two new EZASOH03 codes and ships `maclib/nsfezasm.mac` rather than
   editing Shelby's macro.
3. **Stub-verb ERRNO: ENOSYS → EOPNOTSUPP.** M3-2's `NSF_ENOSYS = 78` was
   wrong (Table 67 has no ENOSYS; 78 is EDEADLK). The stub verbs now
   complete with `NSF_EOPNOTSUPP` (45); `NSF_ENOSYS` is deleted and
   tombstoned in `include/nsfreq.h`.
