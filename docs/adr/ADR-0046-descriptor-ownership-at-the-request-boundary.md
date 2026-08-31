# ADR-0046 — Descriptor ownership at the request boundary

**Status:** Accepted (M5-2d1)
**Date:** 2026-08-31
**Supersedes / amends:** discharges the *socket ownership by ASID* half of
spec §17.3; ADR-0041 (the NSFRQE crossing) is the mechanism this sits on.

---

## 1. Context — what was wrong

Since M5-2a the transport has carried a real `NSFRQE` from an unauthorised
client in another address space into `nsfreq_dispatch`. Nothing on the way in
established that the **socket** named by `r->sockdesc` belonged to the client
naming it.

`req_socket()` was `sock_lookup(r->sockdesc)` and nothing else. `sock_lookup`
validates the table index and the slot generation, and **nothing about who is
asking**.

**And the descriptor is not merely forgeable — it is guessable.**
`g_socktab` is one array; `sock_alloc` returns the lowest free index regardless
of which app asks; the descriptor is `(gen<<16)|idx` with `idx` in 0..63 and
`gen` starting at 0 on a fresh STC. A client calibrates against its own
descriptors and walks the neighbours — tens of attempts, not 2³².

**There were two resolution points and only one was obvious.** `sel_scan`
(SELECT) resolves **one descriptor per mask item** and never goes near
`req_socket`, so it probes many descriptors per request. It is the wider door,
and it is the one nobody had looked at.

---

## 2. Decision

**One check function, `nsfreq_sock_owned(desc, ascb, asid)`, with exactly two
callers:** `req_socket` (`src/nsfreq.c`) and `sel_scan` (`src/nsfsel.c`).

### 2.1 It resolves AND checks, in one call

A function that hands back a `SOCKCB` for the caller to validate is how this
hole came to exist. `nsfreq_sock_owned` performs the lookup itself, so **a
caller cannot hold a socket the check did not clear.**

### 2.2 The input is the captured caller identity — NOT `r->apptok`

Two independent reasons, and the second is the one that was measured:

1. **`r->apptok` is client-supplied.** It rides in the NSFRQE at +56 and
   `nsfreqx_dispatch_in` rewrites only `ecb`, `ubuf` and `ulen`, so it crosses
   the boundary unchanged. Comparing `s->apptok` against `r->apptok` compares a
   value the client chose with another value the client chose. That is not a
   check.
2. **`r->apptok` IS NOT POPULATED ON MOST REQUESTS.** The EZASOKET facade sets
   it on exactly three verbs — INITAPI reads it back, SOCKET and TERMAPI set it
   — and leaves it zero everywhere else, because until now nothing downstream
   read it. A token-comparing check therefore refuses BIND, CONNECT, SEND, RECV
   and the rest **for every honest client**.

   This was not reasoned out in advance. It was **measured**: the first
   implementation compared `s->apptok` against `r->apptok`, and
   `test_roundtrip` in TSTREQ **deadlocked** — a blocking RECVFROM was refused
   `EBADF` before it could park, so the waiter was never woken. The stack
   would have been broken for every real application.

The direction that works uses nothing the client can write: **the socket names
its owning app slot; that slot records the address space that opened it; that
is compared against the `(ascb, asid)` the SVC routine captured from the FLIH
at `CLAIMOK`.**

### 2.3 Scope is PER ADDRESS SPACE, not per app instance

Two `INITAPI`s from one address space may use each other's sockets.

This is deliberate. One address space is one protection key and one storage
image, so those two instances can already read each other's memory directly —
separating their sockets buys nothing an attacker cannot trivially bypass. The
threat model is *cross-address-space*, and that is exactly the boundary drawn.

It is also the only direction that is well-defined: `identity → token` is
**one-to-many** (an AS may INITAPI repeatedly), so a reverse lookup has no
unique answer. `socket → token → identity` has exactly one.

#### TWO SCOPES, DELIBERATELY DIFFERENT — do not align one to the other

This is the paragraph that exists to prevent a later "fix". The system now
scopes two different things two different ways, **both correct**:

| what | scoped by | why |
|---|---|---|
| **teardown** (`RQ_TERMAPI` → `term_one` → `soc_destroy`) | the **token** | Two `INITAPI`s from one address space share an ASCB. TERMAPI from app 1 must **not** destroy app 2's sockets. The M5-2c memo kept the token for exactly this reason. |
| **access control** (`nsfreq_sock_owned`) | the **address space** | That is where the protection boundary actually is — one AS is one key and one storage image, so a finer split buys nothing an attacker cannot bypass by reading memory directly. |

They do not conflict. But someone who finds only one of them will be tempted to
align it to the other, and **either direction of that "fix" is a defect**:

- scoping *teardown* by address space would make one app's TERMAPI destroy a
  sibling app's sockets;
- scoping *access control* by token would reintroduce the dependency on
  `r->apptok` — the field §2.2 shows is populated on three verbs of about
  twenty, and which the client supplies anyway.

The permissive case is **pinned by an assertion that allows it**
(`test_socket_ownership`, "same address space, second instance: ALLOWED"), so
if this ever does need to be tighter, reversing it costs one named line and the
test says so out loud rather than silently going red.

### 2.4 A foreign descriptor is INDISTINGUISHABLE from an unknown one

Both return `NULL`, **through the same `return` statement**, so every one of
the nine `req_socket` callers maps them identically (all nine → `NSF_EBADF`)
and no caller *can* tell them apart. For SELECT the foreign entry takes the
existing `s == NULL` branch: silently not-ready, count unaffected, **the rest of
the mask served normally**, and no error on the call.

The alternative — a distinguishable refusal — turns any verb, and SELECT
especially, into an **existence oracle for descriptor numbers**, which is worth
more to an attacker than the individual socket. **No new errno is introduced
anywhere.** NSF's existing POSIX divergence for invalid descriptors is a
conformance question for `docs/ezasoket-conformance.md`, not for a security
step: a change that quietly alters a verb's errno semantics is two changes
under one name.

### 2.5 The token is still authenticated, for the two verbs that consume it

`nsfreq_dispatch_id` authenticates `r->apptok` against the captured identity
once, at the boundary, and zeroes it if it is not this caller's —
producing the **existing** invalid-token failure (`app_index` rejects token 0
by construction), not a new one.

This is not the ownership check; it protects the **stamp**. `do_socket` does
`s->apptok = r->apptok`, so without it a client could not *use* a foreign
socket but could still *create* one labelled as another app's — and the
ownership check would then resolve that socket to the wrong address space for
the rest of its life. `RQ_INITAPI` is exempt **explicitly, not by ordering**:
it arrives with no token and *writes* `apptok` as its output.

### 2.6 An enumeration guard, because forgetting is the actual failure mode

`tools/check-sock-lookup.sh` (CI job `sock-lookup-callers`) enumerates every
`sock_lookup` call site in `src/` and `include/` and fails the build unless each
carries `SOCK_LOOKUP: CHECKED <why>` or `SOCK_LOOKUP: INTERNAL <why>`.

The rule this encodes is not "add a check". It is: **a new resolution point
must be classified by a person.** `req_socket` never had a check and nobody saw
`sel_scan` — a rule that lives only in a review note fails the same way twice.
Same family as `check-card-columns.sh`: runs before the toolchain, needs nothing
installed.

It carries its **own** positive control: finding *zero* call sites exits 2 with
"the search is broken, not the tree", because a broken pattern and a clean tree
are otherwise identical (CLAUDE.md §8.5).

Scope is production code only. Tests resolve descriptors directly all the time
(22 sites across four files) precisely because they are probing the table; they
carry **no marker at all**, so nobody can mistake one for guard coverage.

---

## 3. The one piece of hidden state, and why it fails closed

`req_socket(r)` takes only `r`, and threading `(ascb, asid)` through nine
handlers is invasive, so the dispatching identity is a module static
(`g_cur_ascb` / `g_cur_asid`) set at the top of `nsfreq_dispatch_id`.

Safe for a stated reason: the executive is single-task and run-to-completion,
and ADR-0042 §10 permits exactly **one** cross-AS request in flight, so
`nsfreq_dispatch_id` is never re-entered while a dispatch is open.

**It is deliberately NOT cleared on exit**, and that is a decision about failure
*direction*. The one misuse is consulting it while completing a request
belonging to a different client than the one being dispatched. Nothing does
that today. If something did:

- left **set** → B's identity vs A's socket → mismatch → **denied**. Fail-closed,
  and it surfaces as a visible bug.
- **cleared** to `(0,0)` → "no identity" → check skipped → **allowed**.
  Fail-open, and silent.

The safe-side asymmetry this milestone bends around everywhere says: keep it set.

**SELECT does not read it at re-scan time at all.** `nsfsel_on_notify` runs with
*no dispatch open*, so it would be reading an unrelated client's identity rather
than merely a stale one. The identity is captured into the `SELCB` when the
SELECT parks (`cb->ascb` / `cb->asid`) and every later re-scan runs as the
client that parked it.

---

## 4. Phase 1

A zero identity means "no address space to ask about", and — the fourth time
this milestone has needed saying — **may never be turned into a verdict.** The
enforcement is a single early return in `nsfreq_sock_owned`, the shape c1
established in `nsfreq_app_classify`.

`nsfreq_dispatch(r)` is `nsfreq_dispatch_id(r, 0, 0)`, so every Phase-1 call
site and every direct-call test skips the check entirely and behaves exactly as
before.

---

## 5. Consequences

- `sock_lookup` **keeps its signature**; `nsfsoc.c` and everything below it
  learn nothing about callers, transports or address spaces. The check lives in
  the two files that already know what a request is.
- `SELCB` grows two words (`ascb`, `asid`). It is a fixed static pool — no
  allocation.
- **`NSFRQE` is untouched and stays frozen at 64 bytes**; the anchor layout does
  not move and `ANCVERNO` stays 3. Applications relink only.
- Two new externals: `nsfreq_sock_owned` (`NSFRQSOW`), `nsfreq_caller_id`
  (`NSFRQCID`).
- A socket whose owning app slot has been released resolves to `NULL` for
  everyone — including a Phase-2 client that still holds the descriptor. That
  is the same fail-closed direction, and it is reachable today only after
  TERMAPI, which destroys those sockets anyway.

## 6. What this does NOT do

- It does not reject probe verbs (`ECHO`/`XFER`/`QUERY`/`UNSTAGE`/`SLOT`) —
  that is **c3**, which retires them. Issue #67 moved there once d0 established
  no (e) role can reach them.
- It does not separate app instances **within** one address space (§2.3).
- It does not change any errno, any SELECT conformance behaviour, or the
  `nsfsoc.c` internal completion lookup.
- It does not address UNKNOWN-at-shutdown (**d2**, a dated open item).
