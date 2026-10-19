# ADR-0016 — A shared `nsftime` seam for the monotonic clock and current task id

**Status:** Accepted (2026-07-07)
**Refines:** Architecture Specification §7.2 (NSFTRC trace entry), and forward
to §6 (NSFTMR)

## Context

The Trace Facility (M0-4, spec §7.2) stamps every ring entry with two platform
facts: a timestamp and the id of the task that wrote it. Neither is portable —
each is answered differently on the MVS target and on the host test build:

- **Timestamp.** On MVS the natural source is the `STCK` instruction (the 64-bit
  TOD clock). On the host there is no `STCK`; a native clock call
  (`gettimeofday`) is used instead.
- **Task id.** On MVS the running task is identified by its TCB address
  (`PSATOLD`, PSA+X'21C'). On the host the single-threaded test build has no
  TCB; the mainline task is simply id `0`.

Two design questions had to be settled before writing the trace entry:

1. **Where does this platform code live?** The flat layout (ADR-0014) keeps
   `asm/*.asm` for genuinely MVS-only code and swaps each such CSECT for a
   `src/*_host.c` shim on the host via the `[host].replace` map — exactly the
   mechanism NSFXQ already uses for the exit→mainline handoff.
2. **Is the clock trace-private, or shared?** NSFTMR (M0-5, spec §6) needs a
   timestamp too. Putting `nsf_now` inside NSFTRC would force NSFTMR either to
   depend on the trace module or to grow a second, parallel clock primitive.

A related constraint from §7.2: the trace "task" field is stored as a **numeric
`UINT`, never a pointer**, so `TRCENT` stays pointer-free and its 128-byte size
is identical on host and target.

## Decision

**Introduce one shared platform seam, `nsftime`, exposing two primitives:**

```c
void nsf_now(NSFTIME *out);   /* 8-byte, pointer-free timestamp */
UINT nsf_taskid(void);        /* current task as a numeric id   */
```

- `include/nsftime.h` declares both and the 8-byte `NSFTIME` (two `UINT`
  halves; `NSF_SIZE_ASSERT(NSFTIME, 8)`), so it embeds directly in `TRCENT`.
- `asm/nsftime.asm` implements them on MVS: `STCK` into `*out`, and `PSATOLD`
  → `R15` for the task id. Like `asm/nsfxq.asm` it is **deferred/unverified** on
  the target until the M0-6 on-MVS suite; a `STATUS` block and an M0-6 checklist
  record what to confirm (symbol mangling, STCK operand, PSATOLD fetch access).
- `src/nsftime_host.c` implements them on the host: `gettimeofday` split into
  the two `NSFTIME` halves, and `0` for the task id.
- `project.toml` swaps `asm/nsftime.asm` → `src/nsftime_host.c` in the
  `[host].replace` map, the same pattern as NSFXQ.

`nsf_now` is **not** trace-private. NSFTMR reuses it at M0-5.

## Consequences

- **`nsf_now` is a shared primitive with a documented cross-platform caveat.**
  Its epoch and scale are platform-specific — MVS returns a monotonic `STCK`
  TOD value; the host returns a wall-clock reading. It is valid for ordering
  trace entries and for relative timing *on one platform*. Callers must **not**
  assume a shared tick unit across platforms, nor derive wall-clock time from
  it. NSFTMR's own tick (100 ms, spec §6.3, ADR-0011) is driven by `STIMERM`,
  **not** by `nsf_now`; the two are independent.
- **`nsf_taskid` returns a numeric id, never a pointer.** This is what keeps
  `TRCENT` pointer-free (§7.2). The host value `0` means "the single mainline
  task"; real per-exit ids matter only once asynchronous exits write trace,
  which is an M1 concern (see the NSFTRC single-writer note).
- **One asm file carries both primitives.** `nsf_taskid` is a single
  instruction; giving it its own CSECT/shim pair would be ceremony. The unit is
  named for its lead primitive (`nsftime`); its header states up front that it
  is the platform seam for *both* the clock and the task id, so the name is a
  deliberate choice, not an accident.

## Alternatives rejected

- **Trace-private clock inside NSFTRC.** Would make NSFTMR depend on the trace
  module or duplicate the primitive. Rejected: the clock is foundational, not a
  trace detail.
- **GCC inline `asm("STCK ...")` in portable C (`#ifdef __MVS__`).** Would avoid
  a second asm file, but deviates from the established ADR-0014 pattern
  ("`asm/*.asm` never compiles on host; a `[host].replace` map swaps each
  MVS-only CSECT for its `src/*_host.c` shim"), and cc370's inline-asm behaviour
  is unverified. Reconsider only if the seam ever needs to be inlined on a hot
  path.
- **A separate asm file/shim for `nsf_taskid`.** Rejected as ceremony for one
  instruction; co-located in `nsftime` with an explicit header note.
