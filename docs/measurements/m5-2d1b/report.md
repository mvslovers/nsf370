# M5-2d1b — the R8 validation

**Status: implemented, offline gates green. NOT live-verified.**
Branch `m5-2d1b-r8`, cut from **`m5-2d1-ownership`**, not from `main` — **#97 had
not merged** (both #96 and #97 OPEN, `main` at `a85aedb`).

`docs/measurements/m5-2d1b/findings.md` is §1's answer, written **before** any
code. This is what was built on it.

---

## 1. What was closed

The router's 20 result stores (`ST R…,REQ*(,R8)`) run in **key 0** through a
pointer the client supplies, gated only by the `"NSFV"` eyecatcher — which
validates a **pointer, not a key**. A client that stamps `"NSFV"` into **CSA**
got 20 key-0 words written into storage shared with every address space.

Now `TPROT` asks the machine, under the **caller's own key**, whether the caller
may store there. Only CC 0 passes.

## 2. Why `TPROT` and not the obvious alternatives

| candidate | why not |
|---|---|
| private-area range check (`GDA PASTRT/PASIZE`) | **misses LSQA**, which is key 0 and lives *inside* the private window — and a store landing on `TCBPKF` with `rc = 0` sets the task's key to **0** |
| a borrowed-key probe (`SPKA`) | would need a **second** borrowed-key block, which the red line forbids — and it can only say "no" by faulting, through the very block it just refused |
| `LRA` + `ISK` | documented unreliable: the frame moves (CLAUDE.md §3) |

`TPROT` **names** the key as an operand rather than entering it, so **`MOVEOUT`
remains the only block running under a borrowed key** — structurally, not by
care — and the refusal is a clean `rc`.

**Availability proved from primary source with the discriminating control:**
`TPROT` is `GENx370x390x900` in Hercules' `opcode.c:5123`; `MVCDK` — the one
M5-2b0 **measured taking `S0C1`** on this target — is `GENx___x390x900` at
`:5165`. Same table, same SSE format, 42 lines apart.

Encoding derived from `SSE_DECODER` (`instfmts.h:1777`), not recalled:
`E5 01 | B1 D1D1 | B2 D2D2`. The access key comes from operand 2 bits 24-27 —
**the same nibble `SPKA` takes** — so b1's proven caller-key derivation
(`PSATOLD → TCB → IC TCBPKF`) is reused unchanged, as the same register value.

## 3. Two probes, one validation, and why that covers all 20 stores

Every store lies in `[R8, R8+63]` (`REQEYE 0` … `REQSNEW 60`, 4 wide) and 64
bytes straddles at most one page boundary — so the block occupies **at most the
two pages probed**. Two probes, **one decision**, no per-store windows: the c
memo's rule, now provable rather than asserted.

**That argument is pinned in C, because it is the part that rots silently.** If
`NSFV_REQ` ever grows past a page the probes stop covering the stores and
*nothing about the check looks wrong*. `nsfv_r8_probe_covers_block`
(`include/nsfvsvc.h:326`) is named for what it **protects**, and it is
**verified to fire**: forcing it to `<= 32` gives
`error: size of array 'nsfv_r8_probe_covers_block' is negative` and the build
stops.

**No C predicate was written for the CC decision**, deliberately. A
`nsfreqx_r8_ok(cc0, cc1)` that nothing calls would reproduce b3's
"pinned seven times and called nowhere", and worse, it would be a **second
encoding of a rule the assembler implements** — the failure this milestone has
already paid for twice. The span assert is the mirror that has a real failure
mode; the CC rule's gate is the listing.

## 4. THE CC 3 DECISION, re-derived — the first answer was right for the wrong reason

Draft reasoning was *"the client just filled in the whole block, so both pages
are resident"*. **That is false.** `FNECHO` touches only `REQEYE`/`REQFUNC`
(offsets 0-7); nothing obliges a client to touch 56-63, and `REQSEXP`/`REQSNEW`
are probe-only. **A straddling block with a never-referenced tail page is a real
shape**, so rejecting CC 3 would have been an *intermittent false refusal of an
honest caller* — the worst class of bug to debug.

The decision was re-made on the **fault comparison**, not on likelihood:

- `CLI REQLAST(R8),X'00'` references the tail before probing. If the tail is
  genuinely unmapped it faults — **which is exactly what the 20 stores do
  today, only later.** It is not a new failure mode; it moves an existing one
  earlier, which is the whole shape of this change.
- With the reference in place, **CC 3 becomes unreachable** (the `CLC` above
  referenced page 1, the `CLI` references the tail), so rejecting it is a pure
  fail-closed default rather than a live path.

## 5. Placement — the ordering trap

The probe sits **immediately after the eyecatcher and before the anchor
validation**, and that order is load-bearing: `BADANC` **stores
`REQRC(,R8)`** (`asm/nsfvsvc.asm`), so it is one of the 20. Placing the probe
after the anchor block would let a corrupt-anchor path store through an
unvalidated R8.

`BADREQ` is the disposition, and it needs no new bail path — **confirmed from
source, not assumed**: it is `LA R15,RCINVAL; BR R14`, rc in R15 only, which is
exactly right for a block we have just proved we cannot write.

**The eyecatcher stays.** It answers a different question (*is this plausibly
one of our request blocks*) from `TPROT` (*may the caller write it*), and
neither implies the other.

## 6. Gates — offline, all green

| gate | result |
|---|---|
| `as370` | rc 0, **0 diagnostics** |
| emitted bytes | `E50180009000` = `TPROT 0(R8),0(R9)`; `E501803F9000` = `TPROT 63(R8),0(R9)`; `BC 7,BADREQ` → `4770 6504`, base **R6 not dropped to 0**, target `00504` = `BADREQ`; `CLI` → `9500 803F` (disp `03F` = 63); `L R9,PSATOLD` disp `21C` = 540; `IC R3,TCBPKF` disp `01C` = 28 |
| **instruction-stream diff** | **confined to the intended block**: 740 → 750 emitted statements, the diff being **exactly** the 10 inserted lines and nothing else |
| all cards present in listing, in source order | **1323 / 1323, 0 missing** — and **verified to discriminate**: a deliberate 72-byte comment made as370 return **rc 8** and swallow the following `CLI`, which the check named |
| column 71 | OK — **after catching one of my own cards at 72 bytes mid-edit** |
| `nsfv_r8_probe_covers_block` | **verified to fire** (forced to `<= 32` → build stops, by name) |
| `make test-host` | 3469 PASS / 0 FAIL — **a no-regression check only**: `asm/*.asm` never compiles on host and no host test includes `nsfvsvc.h` |
| cross-build | 6 modules clean |

## 7. NOT VERIFIED — live is empty in this step, by design

**Nothing ran on MVS.** The check is a privileged instruction on a
caller-supplied pointer; the host build cannot reach it at all. Its live gate is
item 4 of the combined round.

## 8. What the check does NOT establish

Carried from `findings.md` §5 so it travels with the change:

1. **Point-in-time, not a lock** — keys can change and pages be stolen between
   the probe and the stores.
2. **Writability under the caller's key, NOT ownership** — another task's
   storage in the same address space at the same key is accepted. That residual
   is d1 §2.3's boundary.
3. **It says nothing about what the storage IS** — a client may hand over a
   writable block of its own that overlaps something it cares about.
4. **CC 3 means "could not determine"**, and is unreachable here only because
   the two pages are referenced first (§4).
