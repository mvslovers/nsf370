# `NSFV_REQ_EYE` is EBCDIC — and `4E 53 46 56` was never observed

M5-2d1's second live round (PR #100) stopped §2.4 case 2 on a contradiction it
was right to refuse to explain away:

> the diagnostic's "wanted" bytes printed as `4E 53 46 56`, which is **ASCII**
> `NSFV`, while the staged-and-transformed value implies the source was
> **EBCDIC** `D5 E2 C6 E5`. Both cannot be true of the same literal.

d1c's kickoff made settling it a precondition for quoting any case-2 result: *a
case whose own diagnostic is internally inconsistent cannot certify anything.*

**Settled: the literal is EBCDIC. The ASCII value was derived, not measured.**

## The chain, each step read rather than assumed

**1. What `cc370` emits for the literal.** A one-line translation unit,
`const char probe_eye[5] = NSFV_REQ_EYE;`, compiled with `cc370 -S`:

```
PROBE@EY EQU   *
         DC    C'NSFV'
         DC    X'0'
```

So the literal reaches the assembler as an as370 **character constant**, not as
a byte list the compiler chose an encoding for.

**2. What `as370` assembles `C'NSFV'` to.** From the `-a=` listing of that same
output — the bytes, not a belief about them:

```
000000 D5E2C6E5      87          DC    C'NSFV'
```

**3. What `D5 E2 C6 E5` is.**

| encoding | `N` `S` `F` `V` |
|---|---|
| EBCDIC CP037 | `D5 E2 C6 E5` |
| ASCII | `4E 53 46 56` |

`NSFV_REQ_EYE` on target is **`D5 E2 C6 E5`**.

## Two independent corroborations, neither needing the stand

**The observed transform.** #100 measured the staged block reading
`D6 E3 C7 E6`. `src/nsfv.c:450` adds 1 to each byte of `stage[]`, and
`D5 E2 C6 E5 + 1` per byte is exactly `D6 E3 C7 E6`. An ASCII source would have
produced `4F 54 47 57`. The measurement #100 *did* take already implies EBCDIC.

**Every green Stage-0 run, for a year.** The router validates the caller with
`CLC REQEYE(4,R8),=CL4'NSFV'` — an as370 constant, hence EBCDIC — against bytes
the client `memcpy`'d out of `NSFV_REQ_EYE`. **If the literal were ASCII, no
Stage-0 gate could ever have passed.** `TSTSVC`, `TSTUBUF`, `TSTDEATH`,
`TSTXFW`, `TSTRQXM`, `TSTRQXC` and `TSTRQXF` are all standing evidence.

## So where did `4E 53 46 56` come from?

**It was not observed.** The tell is in #100's own quoted output, and it is
visible without the stand:

```
case 2: target[0..7] = D6 E3 C7 E6 01 01 01 05
```

The line **stops after eight bytes**. The `printf` that produced it
(`test/mvs/tstd1r.c`) carries a trailing `  (wanted %02X %02X %02X %02X)`
segment, and **that segment is not in the captured line at all**. The `wanted`
value was derived by reading the C source with an ASCII mindset and then
reported in the shape of a measurement.

That is the same failure class the round was otherwise careful about, one level
down: **a derived value presented as an observation.** #100 was right to stop —
the contradiction was real — but the half that was wrong is the ASCII half, and
it was never on the machine.

## Consequences for case 2

1. **The `+1` correction is sound.** Staging `byte − 1` yields the literal after
   `src/nsfv.c:450`'s transform. Derive it from `NSFV_REQ_EYE` rather than
   writing `D4 E1 C5 E4`: charset-transparency is a project rule (spec 15.3),
   and a hardcoded value would re-create exactly the assumption this document
   removes. Char arithmetic wraps byte-wise with no carry, so minus-one is exact
   across all 64 bytes, `0x00` included.
2. **The transform is applied by the STC, not by `XFERIN`.** `XFERIN` is a plain
   `MVCK`; the `+1` happens in `src/nsfv.c:450` when the request is serviced. So
   `stage[]` must be read back **after** the POST, which is what #100 did.
3. **Fix the diagnostic so it cannot lie again.** Print the staged bytes and the
   expected post-transform bytes, **both computed from `NSFV_REQ_EYE`**, so the
   line is self-consistent under either encoding and no reader can derive a
   value from the source and mistake it for the output.
4. **The rc alone still cannot attribute the refusal.** `BADREQ` returns in R15
   only and *both* the eyecatcher check and the TPROT check land there. The
   discriminator is the assertion that failed in #100: read the target back and
   require `memcmp(target->eye, NSFV_REQ_EYE, 4) == 0`. That must be a **gate**
   — if the eyecatcher did not land, return the skip code rather than printing a
   PASS underneath it (CLAUDE.md 8.5, and exactly what #100 walked into).
