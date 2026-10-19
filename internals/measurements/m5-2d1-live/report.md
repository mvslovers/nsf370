# M5-2d1 — the combined live round

**Status: 2.1 GREEN (the stop-gate). 2.2 measured and decisive. 2.3 and 2.2b ran
but DID NOT DISCRIMINATE. 2.4 (R8) NOT RUN. 2.6 NOT RUN.**

Branch `m5-2d1-live`, on `main` after #96/#97/#98 merged (`ad01db9` + the
countersign). Stand: MVSCE on mvsdev, CTCI 0500/0501, MTU 1500.

Deploy verified clean each time (`HTTP500 = 0`, 6 modules incl. the changed
`NSFVSVC`). **Zero dumps** (`IEA995I` count 0) across the whole round.

---

## 2.1 THE CROSS-AS ACCEPT — GREEN, and it was the stop condition

This is d1's open item: an **accepted TCP child** is created by the stack
(`tcp_child_create`) and inherits the listener's `apptok`; the client never
named it in a SOCKET call. **A check one degree too strict refuses every
accepted connection** — every server this stack exists for. Phase 1 dispatches a
zero identity so the check is inert there, and TSTREQ's inherited-child case
stamps a socket by hand rather than producing one. Only a real accept from a
real second address space exercises it.

New: `test/mvs/tstd1a.c` (`TSTD1A`) + `samples/host/d1accept_peer.py`. The verb
sequence is `tstezat.c`'s, proven live at M4-5, with `tstrqxm.c`'s cross-AS
source list — so a failure here would be the check, not a freshly written accept
dance. SELECT is deliberately kept out of this gate to keep the axis single.

**Result — batch `CC 0`** (`IEFACTRT … /00000/`), console
`TSTD1A: RECV n=15` → `GATE DONE`, and the host peer independently:

```
sent 15 bytes
PASS: echoed 15 bytes, byte-exact
```

The echo is the gate: it required `getpeername`, `recv` **and** `send` on the
**inherited child** to be permitted. The TSO re-run reports `CC 20` by design —
the one-shot peer was consumed by the batch run, and `CC 20` is the "could not
run" code, not a pass (the TSTRQXM/TSTTCPW precedent).

**The round was allowed to continue.**

---

## 2.2 B DRIVES A'S DESCRIPTOR — measured, and the decisive evidence is WHICH
## descriptor was reached, not how many

`test/mvs/tstd1b.c` (`TSTD1B`), PARM roles. Two **real** address spaces: A holds
a listening socket ~60 s and announces readiness on the console; B is submitted
into that window and sweeps the whole internal descriptor space
`(gen<<16)|idx`, gen 0-1 × idx 0-63 = **128 attempts**, feeding each straight
into an `NSFRQE` — **bypassing its own facade table**, which is the point: the
facade cannot *name* a foreign socket, the transport could.

| arm | STC | sweep | first descriptor reached | B |
|---|---|---|---|---|
| check ON (run 1) | 1713 | 0 hits / 128 | — | CC 0000 |
| **check OFF** (revert) | 1714 | **2 hits / 128** | **`00010000`** | CC 0001 |
| check ON (restored) | 1715 | 1 hit / 128 | `00010001` | CC 0001 |

**The comparison that carries is revert vs restored, on STCs one minute apart
with the identical A-holding setup: `00010000` → `00010001`.** A opened its
socket first on each instance (internal idx 0), B second (idx 1). So with the
check off B reached **A's socket and its own**; with the check on it reached
**only its own**. The descriptor that disappeared is A's.

Also confirmed in both arms, and A's own socket still worked at the end of its
hold — the check did not break the owner while refusing everyone else.

---

## 3. THREE INSTRUMENT DEFECTS, FOUND BY THE ROUND, REPORTED NOT FIXED

Per the round's red line: no fix while measuring.

**3.1 The sweep does not exclude B's OWN descriptor**, so the assertion
`hits == 0` is simply wrong — B legitimately reaches its own socket, and the
restored arm's 1 hit is **correct behaviour reported as a failure** (B `CC 0001`).
It should be `hits <= 1` with the own-descriptor identified, or the sweep should
skip it.

**3.2 The hit COUNT is not comparable across STC instances.** The sweep covers
generations 0-1 only. Run 1 ran on STC 1713, which had already served TSTD1A and
the deploy-run tests, so its slots had advanced past gen 1 and even **B's own**
socket fell outside the swept range — which is why run 1 read 0 rather than 1.
**Run 1's "0 hits" is therefore NOT the clean result it appears to be**, and it
is not evidence against run 3's 1. The revert/restore pair on comparably fresh
instances is.

**3.3 §2.2b and §2.3 did not discriminate.** Both aim at a **hardcoded**
descriptor `0x00000000`, but A's live descriptor was `0x00010000`. So in every
arm those cases were probing a *never-existing* descriptor, not a *foreign* one:

```
foreign rc=-1 errno=9 | unknown rc=-1 errno=9     (identical -- in ALL THREE arms)
SELECT(foreign) rc=0 errno=0 ready=0              (not ready -- in ALL THREE arms)
```

They are green and they are **not evidence**. The indistinguishability property
and the SELECT-foreign property remain **live-unproven**; their host coverage
(TSTREQ, TSTSEL) is what stands behind them. **§2.3's parked path was not driven
at all** — the test uses the poll form.

---

## 4. NOT RUN

- **§2.4 (R8)** — the three cases (no eyecatcher; eyecatcher at an address the
  caller does not own; **the never-referenced tail**, which is the case the CC-3
  correction exists for). Needs a client that builds raw `NSFV_REQ` blocks — the
  `tstrqxc.c` machinery — and another deploy cycle. **The R8 half of d1b is
  therefore still offline-only**, exactly as its own PR said.
- **§2.5's R8 arm** — only the descriptor check was reverted. The `nsfvsvc.asm`
  half was not, so the three-state revert covers one of the two checks.
- **§2.6 (Phase 1 live)** — `S NSF` needs CUU 0500/0501, which NSFS holds, so it
  costs another stop/start cycle. Phase 1's inertness is structural (the router
  does not exist there) and host-pinned, but it was not shown live this round.

---

## 5. One artifact of my own sequencing

`TSTD1BA` job 2963 took **`ABEND SFEF`** at 16.32.07. A holds its socket for 60
seconds; I issued `P NSFS` for the restore deploy inside that window, so when A
woke to make its final call the SVC router was gone. **Not a product fault** —
an orchestration overlap — but it is in the log and is named here rather than
left to be found.

## 6. Stand left

NSFS running on the **restored** build (STC 1715, `SVC 239 STOLEN`,
`NSF001I`), `F NSFS,STATS` reads `BUSY=0 INFLIGHT=0 EXHAUSTED=0 COLLISIONS=0
REAPED=0`, **zero dumps**, no `NSF054W`. `NSF.LINKLIB` holds the restored
modules; TESTLIB holds `TSTD1A` + `TSTD1B`.
