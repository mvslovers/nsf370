# NSF370: the provider contract to the outside

Status: 2026-08-23. Design note, not a decision, **not an ADR** — input for one once M5-2
lands. Companion to `socket-provider.md` (libc370).

Context: libc370 must decide at run time whether to use NSF370 or dyn75/X'75'. For that to
be possible at all, NSF370 has to offer something it does **not** offer today: a stable,
documented, read-only **discovery contract**.

---

## 1. Discoverability — the actual gap

libc370's cheapest detection rung was going to be `ssct_find("TCPIP")`. **That cannot
work:** NSF370 registers no subsystem. The SSI path (ADR-0036) was superseded by the private
SVC (ADR-0038) and the SSI probe was retired (PR #47) — there is no SSCT. The reason is
exactly libc370's use case: `IEFSSREQ` is authorized-only, and unmodified relink-only
programs are unauthorized.

**What does exist** (verified against the code):

| Building block | Current state | Usable for discovery? |
|---|---|---|
| SVC number | `NSFV_SVCNUM` = **239**, `#ifndef`-configurable; the STC steals exactly that number or refuses to start (no fallback) | yes, but only by convention |
| SVCTABLE entry | points at the CSA-resident routine; a *free* slot points at the common "invalid SVC" routine | **yes** — read-only, CVT → SVCTABLE |
| Anchor | address held in `NSFVANCH`, a word **inside the routine**; eyecatcher `"NSFVANCR"` | reachable only *through* the routine |
| Request eyecatcher | `"NSFV"` (`CL4`, checked by the router) | internal; not a discovery contract |

The eyecatchers exist today, but as **internal validation** of the routine's own structures —
not as a contract for third parties. That is what needs adding.

**Proposal:** NSF370 publishes a **discovery eyecatcher at a fixed, documented offset from
the SVC routine's entry point.** Two detection forms then become possible, both read-only,
no storage, no abend risk:

- **Fast path:** read the SVCTABLE entry for 239 → entry point → check the eyecatcher at the
  fixed offset. One comparison.
- **Robust path:** walk slots 200–255, checking the eyecatcher at each entry point. 56
  comparisons, still ~0 cost, and independent of the `NSFV_SVCNUM` a given instance was built
  with.

The robust path also yields the **actual** SVC number, which the caller needs anyway.

---

## 2. What NSF370 guarantees a consumer (current state, verified)

- **EZA-compatible, all three surfaces**: EZASOKET (call-by-name), EZASOH03 (plist), the
  EZASMI macro (`nsfezasm.mac`). This is the project's goal, not a coincidence — unmodified
  applications are meant to run **relink-only**.
- **errno = the classic BSD set, identical to libc370.** Verified: `EBADF 9`, `EINVAL 22`,
  `EAGAIN`/`EWOULDBLOCK 35`, `EINPROGRESS 36`, `EOPNOTSUPP 45`, `ECONNRESET 54` — the same on
  **both** sides. **No translation table is needed for NSF**, which removes the silent failure
  class libc370's note rightly identifies as the most expensive. This holds for NSF only;
  dyn75 fetches errno with a second X'75' call and is unaffected.
- **Real blocking, or `EWOULDBLOCK`** — no `-2` contract, no retry budget, no 4096-byte
  decomposition. The dyn75 peculiarities must not be charged to NSF.
- **Verbs today**: SOCKET, BIND, CONNECT, LISTEN, ACCEPT, SEND, SENDTO, RECV, RECVFROM, CLOSE,
  SHUTDOWN, SELECT, GETSOCKNAME, GETPEERNAME, SETSOCKOPT, GETSOCKOPT, FCNTL, INITAPI, TERMAPI.
- **Absent: GIVESOCKET/TAKESOCKET.** Deliberate in v1 — with a visible consequence: NSF's
  SELECT has **no exception readiness**, because TAKESOCKET would be the only exception source
  (`nsfsel.h`, `nsftcp.c`). Socket handoff is attractive for httpd/ftpd; it is an M6 candidate,
  not a v1 promise.

---

## 3. The number that collides: MAXSOC 64 vs FD_SETSIZE 1024

- NSF clamps `MAXSOC` to **64** (`NSFEZA_MAXSOC`), whatever INITAPI requests (EZASOKET permits
  50–2000; NSF clamps and reports the clamp).
- libc370's public header has **`FD_SETSIZE` 1024**, so the `fd_set` size is compiled into
  every consumer's stack frames.
- The reviewed `socket.c` brings a **third** number: `__jcc_FD_SETSIZE = 256`.

Functionally a 1024-bit `fd_set` against 64 possible sockets is harmless (the upper bits are
never set). The care is needed at the **mask convention**: EZASOKET SELECT masks are laid out
**right-to-left in fullwords**. Anything passing an `fd_set` straight through as an EZASOKET
mask must convert. An error there is silent.

---

## 4. `selectex()` / ECB — NSF is better placed here than expected

libc370's note calls this the largest single divergence: the ECB list in `selectex()` is
meaningful for a native stack and meaningless for dyn75.

For NSF the machinery **exists and is proven**: cross-AS branch-entry POST (`__xmpost` via
CVT0PT01) plus the ADR-0040 liveness guard before every POST. ufsd additionally documents that
such a POST reaches an ECB in the target address space's **private key-8 storage**. A cross-AS
`selectex()` with an ECB list is therefore **feasible**, not merely desirable — but it needs a
design (which ECB, who posts when, what happens on client death) and belongs after M5-2 at the
earliest.

**One rule that design inherits before it starts (ADR-0047).** An ECB list is an array
crossing through `ubuf`, which is the identical fork to `RQ_SELECT`'s item array — and that
one was encoded as an *element count* and cost issue #101: the Phase-2 transport moves
`min(ulen, 2048)` **bytes** and never reads `fn`, so a cross-AS SELECT over N sockets crossed
N bytes to be read as 8N. `ulen` is a **byte length for every verb**; a count belongs in
`p1`/`p2`/`p3`. If the element ever crosses a load-module boundary — and here it would, the
facade linking into the application and the engine into the STC — it needs an
`NSF_SIZE_ASSERT`, because a host test links both halves into one compilation and cannot see
the two sides disagree about the size.

---

## 5. Open questions (NSF side)

1. **Discovery eyecatcher:** will one be published at a fixed offset? What value, what offset,
   stable from which version? *(Blocks libc370's rung 1.)*
2. **SVC number 239: convention or configuration?** Today it is `#ifndef`-configurable. Should
   239 become a **fixed ecosystem convention** (then the fast path suffices), or stay
   configurable (then the scan is mandatory)?
3. **How does a consumer learn about the clamp?** INITAPI reports `maxsno`; is that enough, or
   should discovery expose the 64 directly (version + MAXSOC + capability bits at a fixed
   offset)?
4. **Version/capability byte:** does the contract need a "which verbs can this instance do"
   field? As soon as GIVESOCKET/TAKESOCKET or further options arrive, libc370 must be able to
   detect that without guessing the version.
5. **GIVESOCKET/TAKESOCKET:** no in v1 — does that hold until M6? The answer determines whether
   libc370 needs a permanent "this provider cannot do that" convention (`ESOCKTNOSUPPORT`, see
   the companion).
6. **cross-AS `selectex()` with an ECB list:** its own issue after M5-2 — yes or no?
7. **Multiple NSF instances / restart:** what does a consumer see that checks between `P NSFV`
   and `S NSFV`? Detection is cached (decided once per address space), so a restart during a
   consumer's lifetime is a "latched provider is gone" case. Specify the behaviour — presumably
   an error, as with a crashed stack, and no re-discovery.

---

## References

ADR-0036 (SSI, transport-superseded) / ADR-0038 (private SVC, `NSFV_SVCNUM`, slot steal) /
ADR-0040 (client-death guard, `__xmpost` before the reply) / ADR-0041 (NSFRQE crossing);
`include/nsfvsvc.h` (anchor, eyecatchers `NSFVANCR`/`NSFV`), `asm/nsfvsvc.asm:130-138`
(`NSFVANCH`, the eyecatcher checks), `include/nsfeza.h:86` (`NSFEZA_MAXSOC` 64),
`include/nsfreq.h:81-93` (errno values), `include/nsfsel.h:26` (no exception readiness);
spec §15 (EZASOKET surfaces), `docs/ezasoket-conformance.md`.
Companion: `socket-provider.md` (libc370).
