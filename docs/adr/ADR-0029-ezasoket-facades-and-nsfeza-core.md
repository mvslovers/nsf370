# ADR-0029 — Socket API layering: thin facades over one surface-neutral NSFEZA core

**Status:** Proposed (2026-07-15). Introduced with M3-4 (NSFEZA). **Relates to:**
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

GG24-2561 fixes the API-visible socket descriptor as a **halfword**
(`pic 9(4) Binary`), range 1–1023 on MVS 3.8 (Shelby's limit; z/OS MAXSOC goes
to 65535). Our internal descriptor is a fullword `(gen<<16)|id` with a
generation counter — it cannot be surfaced. Therefore NSFEZA keeps a
**per-application mapping table** (socket number → internal descriptor),
anchored at the INITAPI registration (RQ_INITAPI, M3-2):

- CLOSE clears the entry → any later use of that number draws **EBADF** at the
  facade; the gen-check in the core still catches internal reuse. Stale-handle
  protection is preserved end to end.
- `RQ_SOCKET` **auto-registers** the application when no INITAPI preceded it
  (implicit INITAPI — IBM behavior; Shelby triggers it on SOCKET, SELECT,
  GETHOSTBY*).
- MAXSOC is accepted and **clamped** against the pool limit (SOCKET pool
  default 64); no over-promising.

### ERRNO policy and the known ecosystem divergence

§15 mandates **IBM EZASOKET ERRNO values** (not Unix `errno.h` where they
differ). Pinned finding: `EZASOH03.txt` line 314 hardcodes
`LA R4,61 … hECONNREFUSED` — 61 is the **BSD** ECONNREFUSED, and the `h`
prefix reads as "host". If that generalizes, the existing X'75' ecosystem
already diverges from IBM values today, and an application testing
`errno == 61` would break on an IBM-conformant NSF. This is a real
relink-compatibility conflict:

- NSFEZA core maps to **IBM values** (the §15 contract).
- The divergence is recorded in **`docs/ezasoket-conformance.md`**, which M3-4
  creates (it is the M6 acceptance artifact per §15.2). Until SC31-7187
  Appendix B is extracted, entries are marked *ecosystem-verified, IBM
  cross-check pending* — never guessed.
- Whether the `@@75`/dyn75-compat facade needs a BSD-value ERRNO mapping is an
  **M6 audit question**, answered by what HTTPD/mvsMF actually test, not
  decided here.

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
- SC31-7187-03 Appendix B — ERRNO table; extraction pending (blocker for the
  complete conformance doc, not for this ADR).
