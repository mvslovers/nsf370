# M5-2d1 — the ownership check

**Status: implemented, host-verified, offline gates green. NOT live-verified.**
Branch `m5-2d1-ownership`, based on `main` (`a85aedb`).

Host suite **3414 → 3469 PASS / 0 FAIL** (+55). Everything below the §7 line
was verified on the host or offline; **nothing in this round was run on MVS**,
and §7 says exactly which acceptance items that leaves open.

---

## 1. What was closed

An unauthorised client in another address space could drive another client's
sockets through the documented SVC. Open since M5-2a.

`req_socket()` was `sock_lookup(r->sockdesc)` and nothing else, and
`sock_lookup` validates the table index and slot generation and **nothing about
who is asking**. The descriptor is also *guessable*: one socket table, lowest
free index regardless of who asks, `(gen<<16)|idx` with `idx` in 0..63 — so a
client calibrates against its own and walks the neighbours.

**And there were two resolution points, not one.** `sel_scan` resolves **one
descriptor per SELECT mask item** and never goes near `req_socket`. It is the
wider door and it is the one nobody had looked at — which is why this round
also ships a guard against a third appearing unnoticed.

Design: **ADR-0046**. Spec §17.3 now carries the status of all three surfaces
it named (addresses / lengths / socket ownership).

---

## 2. THE FINDING THAT CHANGED THE DESIGN, and it was found by a test

The kickoff's §1 prescribed: identity → app slot → token → compare against the
socket's `apptok`. **The first implementation did exactly that, and it broke the
stack.**

`test_roundtrip` in TSTREQ **deadlocked**: `build/host/tstreq` sat at 0.0 % CPU
with frozen output, and `sample(1)` put the main thread in
`wait_parked → pthread_cond_wait` while the executive thread idled in
`NSFEVMLP`. A blocking `RECVFROM` was being refused `EBADF` before it could
park, so nothing ever woke the waiter.

**Root cause: `r->apptok` is not populated on most requests.** `src/nsfeza.c`
sets it on exactly **three** verbs — INITAPI reads it back (`:177`), SOCKET
(`:214`) and TERMAPI (`:617`) set it — and leaves it zero on every other verb,
because until now nothing downstream read it. A check comparing `s->apptok`
against `r->apptok` therefore refuses BIND, CONNECT, SEND, RECV and the rest
**for every honest client**.

It would also not have been a check even where the field *is* set, since the
client supplies it.

**The direction that works uses nothing the client can write:** the socket names
its owning app slot, that slot records the address space that opened it, and
*that* is compared with the `(ascb, asid)` the SVC routine captured from the
FLIH. `r->apptok` is not consulted at all — pinned by an assertion in which a
client drives its own socket while presenting **junk** in `apptok` and succeeds.

**This is the round's main lesson and it is not a small one:** the prescribed
comparison was plausible, matched the field names, and was wrong in a way that
only a *threaded* test could show — a deadlock, not a failed assertion. A
suite without `test_roundtrip` would have gone green.

---

## 3. Scope, stated because it is a decision and not a gap

**Ownership is per ADDRESS SPACE, not per app instance.** Two `INITAPI`s from
one address space may use each other's sockets.

- One address space is one protection key and one storage image, so those two
  instances can already read each other's memory. Separating their sockets
  buys nothing an attacker cannot bypass directly.
- `identity → token` is **one-to-many**, so the reverse lookup that would
  separate them has no unique answer. `socket → token → identity` has exactly
  one.

Pinned by an assertion that **allows** it, so the scope is visible in the test
output and flipping it is a one-line change to a named assertion.

### 3.1 Two scopes, deliberately different — do not align one to the other

| what | scoped by | why |
|---|---|---|
| **teardown** (`RQ_TERMAPI` → `term_one`, `src/nsfreq.c:483`) | the **token** | two `INITAPI`s from one AS share an ASCB, so app 1's TERMAPI must not destroy app 2's sockets — the reason the M5-2c memo kept the token |
| **access control** (`nsfreq_sock_owned`) | the **address space** | that is where the protection boundary is |

Both are right and they do not conflict, but someone finding only one will be
tempted to align it to the other, and **either direction of that is a defect**:
teardown by address space destroys a sibling app's sockets; access control by
token reintroduces the dependency on `r->apptok`, the field §2 shows is
populated on three verbs of about twenty. Carried in **ADR-0046 §2.3** as well
as here.

---

## 4. Shape of the change

| piece | where |
|---|---|
| the ONE check (resolves **and** checks) | `nsfreq_sock_owned()`, `src/nsfreq.c` |
| caller 1 | `req_socket()`, `src/nsfreq.c` |
| caller 2 | `sel_scan()`, `src/nsfsel.c` |
| declared internal | `soc_complete`'s lookup, `src/nsfsoc.c` |
| token authentication (protects the `do_socket` stamp) | `app_authenticate()`, `src/nsfreq.c` |
| enumeration guard | `tools/check-sock-lookup.sh` + CI job `sock-lookup-callers` |

`sock_lookup` **keeps its signature**; `nsfsoc.c` and below learn nothing.
**`NSFRQE` untouched and still frozen at 64 bytes**; anchor layout unmoved;
`ANCVERNO` 3. Two new externals (`NSFRQSOW`, `NSFRQCID`). `SELCB` gains two
words in a fixed static pool.

**Foreign ≡ unknown by construction:** both return `NULL` through the *same*
return statement, and all nine `req_socket` callers map `NULL → NSF_EBADF`
identically — so no caller *can* distinguish them. SELECT's foreign entry takes
the existing `s == NULL` branch. **No new errno anywhere.**

One clause the equivalence needs, so nobody reads it as broader than it is:
the check has a **third** NULL return, for a socket whose owning app slot has
been released (`app_index < 0`). That one denies **everyone**, the legitimate
owner included. It is the same fail-closed direction and is reachable today
only after TERMAPI — which destroys those sockets anyway — but it is a real
third case and not part of "foreign looks like unknown".

**The one piece of hidden state fails closed.** `g_cur_ascb`/`g_cur_asid` are
set at dispatch entry and deliberately **not cleared**: a stale read denies
(fail-closed, visible) where a cleared read would skip the check (fail-open,
silent). SELECT does not read it at re-scan time at all — `nsfsel_on_notify`
runs with no dispatch open — and captures the identity into its `SELCB` at park
time instead.

---

## 5. Gates run

| gate | result |
|---|---|
| `make test-host` | **3459 PASS / 0 FAIL** (was 3414) |
| §5.1 inherited child | **run first**, green — **as no-regression only**, see below |
| §5.2 two clients, refusal ≡ unknown | host-pinned, TSTREQ |
| §5.3 SELECT foreign entry, rest of mask served | host-pinned, TSTSEL — **both** the dispatch scan **and** the parked re-scan |
| §5.5 Phase 1 unaffected | pinned in both files (check inert at zero identity) |
| §5.6 guard discriminates | **yes** — added an unclassified caller to `src/nsfudp.c`: `rc=1`, named the file and line; removed it: `rc=0`, "2 call site(s), all classified" |
| §5.7 revert test | **three states, below** |
| alias scan | 246 unique, all ≤ 8 chars, `NSFRQSOW`/`NSFRQCID` present |
| cross-build (cc370/as370/ld370) | 6 modules + test modules, clean |

### 5.7 The revert test, three states, exactly the ownership assertions moving

| state | result |
|---|---|
| check present | 3469 PASS / **0 FAIL** |
| check disabled (one axis: the two comparisons in `nsfreq_sock_owned`) | 3457 PASS / **12 FAIL** — TSTREQ 4, TSTSEL 8 |
| restored | 3469 PASS / **0 FAIL** |

**The middle state renders the vulnerability positively rather than as an
absence** — which is what makes it evidence:

```
FAIL: B on A's descriptor -> refused (got 0, want 9)
FAIL: foreign and non-existent return the SAME errno (got 0, want 9)
FAIL: B presenting A's STOLEN TOKEN and A's descriptor -> still refused (got 0, want 9)
FAIL: SELECT counted exactly ONE ready socket (got 2, want 1)
FAIL: A's socket is silently NOT READY for B (got 1, want 0)
FAIL: A's readiness does NOT complete B's parked SELECT (got 1, want <PARKED>)
FAIL: ...and the SAME poke leaves B's parked (owner is honoured) (got 1, want <PARKED>)
```

`got 0` is **success** — B drove A's socket. SELECT `got 2` — B saw A's socket
as ready in its own mask. The last two are the **parked re-scan**: `got 1` means
B's *parked* SELECT was completed by A's socket becoming ready. Every control
assertion (A drives its own socket, B's own socket served, the same poke
completing A's own parked SELECT, Phase 1) **passed in all three states**, so
only the ownership assertions moved.

---

## 6. Two process notes

- The revert arm had to be made to **compile** before it could discriminate:
  removing the check orphaned `idx` and then the `asid` parameter under
  `-Werror`. A revert arm that fails to build proves nothing, and "6 tests
  FAILED" looked at first glance like the discrimination result it was not.
- A `python3` block replacing a multi-line source region **asserted** every
  target before writing. One assertion fired (the `sel_scan` block), the file
  was left untouched, and the edit was re-applied in four separately-asserted
  pieces. Without the assert this would have been the silent no-op edit
  CLAUDE.md §8.5 lists.

---

## 7. NOT VERIFIED — what a live round still owes

**No part of this ran on MVS.** In particular:

1. **§5.1 is met as NO-REGRESSION, not as a test of the check, and acceptance
   item 4 should be read that way.** What ran is (a) a socket stamped by hand to
   model `tcp_child_create`, in TSTREQ with the dummy protocol — that models the
   *stamp*, it is not an accepted TCP connection — and (b) TSTTCP's real accept
   path, green at 841, which runs at **zero identity** so the check is inert
   there too. The case that exercises an inherited child *under* the check is a
   **cross-AS accept**, which is live only. A green host suite must not be read
   as covering it.
2. **§5.2 with two real address spaces.** The host pins the property with
   synthetic identities; the two-AS rig from (e)'s stage a is the live vehicle.
3. Everything in `src/nsfreq.c` and `src/nsfsel.c` is in the Phase-1 `NSF`
   module, so **every host assertion here is about the zero-identity path
   unless it passes an explicit identity** — which is why the new tests drive
   `nsfreq_dispatch_id` directly.

## 8. Housekeeping

`docs/measurements/m5-2d0/` (the d0 survey) is **untracked** and was carried
across the branch switch. It is the previous step's deliverable and is not part
of this change; it needs committing wherever Mike wants it.
