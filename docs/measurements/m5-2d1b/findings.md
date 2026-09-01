# M5-2d1b §1 — what an R8 check can establish, answered before building

Branch `m5-2d1b-r8`, cut from **`m5-2d1-ownership`** and not from `main`, because
**#97 has not merged** (both #96 and #97 were still OPEN at the start of this
step, `main` at `a85aedb`). Everything below is from source; nothing ran on MVS.

**Verdict: a meaningful check IS constructible, and it is stronger than the
range check d0 implied.** §2 below builds it.

---

## 1. Where the router stands, and what it holds

`asm/nsfvsvc.asm:353` — `LR R8,R1` — so **R8 is the caller's plist pointer,
entirely client-supplied**, and the only thing standing in front of the 20
result stores today is the eyecatcher at `:355`
(`CLC REQEYE(4,R8),=CL4'NSFV'`), which validates a *pointer* and not a *key*.

The router runs **in the caller's address space, supervisor state, key 0**, and
already derives the caller's TCB from `PSATOLD` for b1's write-out window
(`:943-946`). So the question is not what it could reach with new machinery; it
is what instrument answers *"may the caller store here?"* directly.

## 2. The instrument: `TPROT`, and it is available on this target

`TPROT` (Test Protection) answers exactly that question, by condition code, and
**without faulting**.

**Availability, from primary source, with the discriminating control.** In
Hercules' `opcode.c` on this stand:

```
5123: /*E501*/ GENx370x390x900 ( "TPROT" , SSE , ASMFMT_SSE , test_protection )
5165: /*E50F*/ GENx___x390x900 ( "MVCDK" , SSE , ASMFMT_SSE , move_with_destination_key )
```

Two entries in the same table, 42 lines apart, same SSE format. `MVCDK` is the
one **M5-2b0 measured taking `S0C1`** on this target — and its gate is
`GENx___x390x900`. `TPROT`'s is `GENx370x390x900`. That contrast is the reason
to believe the availability claim rather than the mnemonic's existence in the
file.

**Semantics**, `control.c:7838-7919`:

| | |
|---|---|
| `PRIV_CHECK` | privileged — the router is supervisor state key 0 ✓ |
| operand 1 | the address tested (`translate_addr(effective_addr1, b1, …)`) |
| operand 2 | supplies the **access key**: `akey = effective_addr2 & 0xF0` |
| CC 0 | not protected — fetch **and store** permitted under that key |
| CC 1 | store protected |
| CC 2 | fetch protected |
| CC 3 | translation exception (not currently translatable) |

**It never program-checks on a bad operand 1** — a translation failure is CC 3,
not an abend. That is the property that makes it usable on a hostile pointer.

**The key convention matches what is already proven.** `akey = ea2 & 0xF0` takes
the same nibble `SPKA` takes (bits 24-27), so the caller's key derivation b1
already validated — `PSATOLD → A(caller TCB) → IC R3,TCBPKF(,R9)`, high nibble,
verified against `SYS1.AMODGEN(IKJTCB)` — is reused **unchanged, as the same
register value**. No new derivation, no new chase, no new privileged step.

**`as370` does not know the mnemonic**, so it must be raw bytes — the `MVCK`
precedent. Measured, with `SPKA` as a positive control in the same assembly:

```
         TPROT 0(8),0(9)
 ERROR: Undefined operation code in line 2 - TPROT
000000 B20A 9000      00000    3    SPKA  0(9)
```

**Encoding, derived from the decoder and not from memory** (`instfmts.h:1777`,
`SSE_DECODER`): `b1 = temp>>28`, `d1 = (temp>>16)&0xfff`, `b2 = (temp>>12)&0xf`,
`d2 = temp&0xfff` over the fullword at `inst+2` — i.e.
`E5 01 | B1 D1D1 | B2 D2D2`. So `TPROT 0(R8),0(R9)` is **`E501 8000 9000`**.

## 3. It needs no borrowed key — the red line holds structurally

`TPROT` **names** the key as an operand rather than entering it. There is no
`SPKA` pair, so **`MOVEOUT` remains the only block in the module running under a
borrowed key**, and it remains so by construction rather than by care.

This is also why the check can *return a clean rc*: a borrowed-key probe would
have had to fault to say "no", and the routine cannot then report through the
very block it just proved it cannot write.

## 4. ONE validation covers all 20 stores — provably, not by assertion

The c memo's rule was *one validation of R8, not twenty `SPKA` windows*. It still
holds, and it can now be proved rather than asserted:

- Every one of the 20 stores is `ST Rn,REQ*(,R8)`; the field EQUs run
  `REQEYE 0` … `REQSNEW 60`, each 4 bytes wide, so **every store lies within
  `[R8, R8+63]`**.
- `NSF_SIZE_ASSERT(NSFV_REQ, 64)` fixes that span at 64 bytes.
- 64 bytes straddles **at most one** 4 KB page boundary, so the block occupies
  **at most two pages** and never three.

So probing **byte 0 and byte 63** covers the exact footprint of all 20 stores.
Two probes, **one decision**, no per-store windows.

This is the part most likely to rot silently: if `NSFV_REQ` ever grows past 64
bytes, or a field is added above offset 60, the two probes stop covering the
stores **and nothing about the check would look wrong**. §2 pins that in C.

## 5. WHAT THE CHECK CANNOT ESTABLISH

Stated precisely, because a check weaker than it looks is worse than a
documented gap — the gap gets closed and the weak check gets trusted.

1. **It is point-in-time, not a lock.** Storage keys can be changed and pages
   stolen and rebound between the probe and the stores. Nothing here holds the
   storage. This is the same family as the `LRA`+`SSK` caveat already in
   CLAUDE.md §3.
2. **It establishes writability under the caller's key — NOT ownership.** Any
   storage in the caller's key is accepted, including another task's storage in
   the same address space at the same key. That residual is exactly d1's §2.3
   boundary: one address space is one key and one storage image.
3. **It says nothing about what the storage IS.** A client may hand over a block
   of its own that overlaps something it cares about. That is the client harming
   itself, and no check at this boundary can distinguish it.
4. **CC 3 means "could not determine", not "bad address".** A valid but
   currently-paged-out address gives CC 3. §2 rejects on CC 3 — fail-closed —
   which means a legitimate client whose block straddles a page boundary with a
   paged-out tail would be refused. It gets `rc = INVALID` and can retry; it is
   not abended and nothing is corrupted. The alternative (forcing the page in
   with a reference first) buys that case at the cost of a **new fault point**
   on an unmapped tail, which is a worse trade at this boundary.
5. **It does not make the eyecatcher redundant.** The eyecatcher stays. It
   answers a different question — *is this plausibly one of our request blocks*
   — and `TPROT` answers *may the caller write it*. Neither implies the other:
   a client can stamp `"NSFV"` into storage it owns and still not be our caller,
   and our caller's block is writable whether or not the stamp is there.

## 6. What this closes, and what it does not

**Closes:** the escalation in which a client points R8 at **common storage**
(CSA/SQA/nucleus/LPA) that happens to contain `"NSFV"`, and the router — key 0 —
writes 20 words of result into storage shared with *every other address space*.
`TPROT` under the caller's key returns CC 1 there and the request is refused.

It also closes the sharper variant a private-area range check would have
**missed**: `LSQA` is key 0 and lives *inside* the private area window
(`GDA PASTRT/PASIZE`, `X'10'`/`X'14'`, measured `[090000..9E0000)` in 40-IDENT),
so a range check would have accepted an R8 aimed at the caller's own LSQA — and
a store landing on `TCBPKF` with `rc = 0` would set the task's key to **0**.
`TPROT` rejects it, because the caller cannot write its own LSQA either.

**Does not close:** §5's four residues, and in particular a client aiming R8 at
its own key-8 storage in a way that damages itself.
