# 80-CHK — does a data-returning cross-AS receive store into key-0 CSA from key 8?

**Answer: yes. `S0C4`, reproduced twice, with a control that isolates the store.**

Issue #80 is **confirmed**. This round measured; it fixed nothing, and the fix is a
separate step with its own design decision.

Branch `m5-80-chk-recv-key`, base `main` at `5bc9abb` (post-#84). Stand: MVSCE on
`mvsdev`, CUU 0500/0501 online, `tun0` up with a single route.

---

## 0. Precondition — the data path exists

The previous round could not run `TSTRQXM` at all: Hercules failed to create the TUN,
MVS IPLed without 0500/0501, and re-`attach` left MVS answering `HAS NO LOGICAL PATHS`.
This round is entirely about the cross-AS **data** path, so it began by proving the
instrument exists.

`make test-mvs ARGS="--only TSTRQXM"`, job **MBTTEST JOB02857**:

```
  TEST       BATCH          TSO
  ---------- -------------- --------------
  TSTRQXM    ok CC 0        FAIL CC 1
```

Host peer (`samples/host/shortwrite_listener.py`):

```
connected from 192.168.200.1:49152
received 9353 bytes
PASS: 9353 bytes byte-exact against the pattern
```

Batch is the gate and it is **CC 0**. The two TSO failures are the documented by-design
ones, checked rather than assumed — the one-shot listener was consumed by the batch run,
so `CONNECT` gets `errno 61` and `CLOSE` fails with it:

```
  NOTE: CONNECT failed rc=-1 errno=61 -- is shortwrite_listener.py running on the host?
  FAIL: CONNECT across the boundary (the first PARKED request to complete) (got -1, want 0)
  FAIL: CLOSE the stream socket (host sees the FIN) (got -1, want 0)
```

`nsfsx_stop` was never involved; the device came up on every NSFS start of the round
(`NSF210I CTCI 0500/0501 UP DD SYS00003/SYS00005 MTU 1500`).

---

## 1. The chain, re-derived from source

Traced against `main`, not taken from the issue. Four steps, and the last one is the
instruction.

**1. The private NSFRQE's `ubuf` is pointed into CSA.** `src/nsfreqx.c:62`, in
`nsfreqx_dispatch_in`:

```c
    priv->ubuf = stage;
    priv->ulen = staged;
```

`stage` is `slot->stage` (`src/nsfsx.c:1110`), inside the anchor allocated by
`getmain(sizeof(NSFV_ANCHOR), 241)` — **subpool 241, key 0**.

**2. The dispatch runs outside the key window.** `src/nsfsx.c:1152` calls
`nsfreq_dispatch_id` *after* `__prob(savekey, NULL)` at `:1171`, and the source says why
in as many words:

```c
            /* Dispatch OUTSIDE the key window: the executive runs in its own
            ** key 8 on ordinary storage, exactly as in Phase 1. */
```

**3. Nothing in the protocol layer re-enters key 0.** A grep for
`__super|__prob|SPKA|PSWKEY0` across `src/*.c` excluding the four Phase-2 files
(`nsfsx.c`, `nsfv.c`, `nsfsmain.c`, `nsfswap.c`) returns **nothing**. This is the
load-bearing negative: every other CSA write in the design sits inside a key window, and
this one does not.

**4. The store.** For UDP, `src/nsfudp.c:198-201` in `udp_complete_recv`:

```c
    if (r->ubuf != NULL && r->ulen > 0u) {
        want = (paylen < (USHORT)r->ulen) ? paylen : (USHORT)r->ulen;
        got  = buf_copyout(bpay, r->ubuf, want);
    }
```

and the instruction itself is one level down, `src/nsfbuf.c:285`:

```c
            memcpy(d + total, b->data, take);
```

TCP reaches the identical `buf_copyout` from `tcp_recv_drain_to`
(`src/nsftcp.c:628` `UCHAR *dst = (UCHAR *)r->ubuf;`, `:639` the call).

**Why the send direction has always worked.** M5-2b0 measured the asymmetry: CSA is key 0
and **not fetch-protected**, so a key-8 *fetch* succeeds and a key-8 *store* faults. A
send **reads** `ubuf` (`src/nsftcp.c:610`, `:2019`) — that is where `TSTRQXM`'s 9353 bytes
go. A receive that returns data is the mirror image, and **no test in this tree had ever
driven one**.

---

## 2. The control, and why it is the one that discriminates

"NSFS abended `S0C4`" on its own does not separate K(i) from K(iii): a bad pointer, a
malformed request, or a fault elsewhere would all present the same way. The control has
to differ from the arm by the **store** and by nothing else — and it does, for free,
because of the guard quoted above.

A **zero-length datagram** gives `paylen == 0`, hence `want == 0`. The guard is still
TRUE (the client asked for 512 bytes, so `r->ulen > 0`), the call is still made, and
`buf_copyout`'s own loop — `while (b != NULL && total < n)` — simply never runs:

| | crossing | `udp_complete_recv` | `buf_copyout` | `memcpy` |
|---|---|---|---|---|
| **control** — 0-byte datagram | same | same, called | called with `n = 0` | **not reached** |
| **arm** — 256-byte datagram | same | same, called | called with `n = 256` | **runs** |

One line apart, on one code path, in one function. `TSTUBUF` does not discriminate here
and neither would a send/receive comparison; this does.

**Both shapes reach the same store.** `udp_complete_recv`'s own header comment says it is
*"shared by the parked-RECV and rxq-dequeue paths"*, so the inline shape (data already
queued when the RECV is issued) and the parked shape reach the identical instruction.
This round drove the **parked** shape, because the peer's delay makes it deterministic
with no guest-side timing. The inline shape was **not driven live** — see §7.

---

## 3. The arm, live

`test/mvs/tstrqxr.c` (`TSTRQXR`, host = false) with `samples/host/recvkey_peer.py`.
The client is **unauthorised** (`__isauth() == 0`, asserted) and carries no stack of its
own: every socket op executes in the NSFS address space.

Run twice — once on the build that produced the finding, once on the **final committed
binary** after the probe was gated behind `PARM='ARM'` (§6), so the artifact and the
evidence are the same thing. Both runs are identical line for line.

**Run 2, JOB02864 against STC 1593** (the committed binary):

```
10.51.49 JOB 2864  +TSTRQXR: 80-CHK CROSS-AS RECEIVE KEY PROBE START
10.51.49 JOB 2864  +TSTRQXR: control 1 -- SEND direction
10.51.49 JOB 2864  +TSTRQXR: control 2 -- ZERO-BYTE RECV issued (no store)
10.51.52 JOB 2864  +TSTRQXR: control 2 -- ZERO-BYTE RECV RETURNED n=0
10.51.52 JOB 2864  +TSTRQXR: ARM -- DATA RECV ISSUED (len=512, expecting 256 bytes)
10.51.55 STC 1593  NSF900E NSF ABEND INTERCEPTED -- CAPTURING AND PERCOLATING
10.51.55 STC 1593  NSF902I RECOVERY ENVIRONMENT: SUP=N AUTH=Y
10.51.55 STC 1593  NSF903I NSFS TRANSPORT QUIESCED BY RECOVERY -- SVC RESTORED, CSA AND ROUTER RETAINED
10.51.55 STC 1593  NSF901I NSF EMERGENCY TEARDOWN COMPLETE
10.51.55 STC 1593  IEF450I NSFS NSFS - ABEND S0C4 U0000 - TIME=10.51.55
10.51.55 STC 1593  IEF404I NSFS - ENDED - TIME=10.51.55
```

**`ZERO-BYTE RECV RETURNED n=0` is present. `ARM -- DATA RECV RETURNED` is absent.**
That pair is the result.

Run 1 (JOB02859 against STC 1592) is the same sequence at 10.43.43–10.43.49, likewise
ending `IEF450I NSFS NSFS - ABEND S0C4 U0000`.

**The datagram really was delivered.** The peer log, both runs:

```
trigger 1 from 192.168.200.1:7788 -> b'\xd9\xf0'
  sent ZERO-LENGTH reply (the control)
trigger 2 from 192.168.200.1:7788 -> b'\xd9\xf1'
  sent 256-byte reply (the arm)
```

`\xd9\xf0` / `\xd9\xf1` are EBCDIC `R0` / `R1` — the guest's own triggers, so the send
direction was alive at both points and the arm's 256 bytes were genuinely put on the
wire. The fault is on receiving **real data**, not on a datagram that never arrived.

### The readings (kickoff §2), from the retained anchor

`NSF903I` retains the CSA, and a retained anchor is readable through `/.dm` after its
address space is gone (the m5-79 technique). Anchor `00AAD7C8`, slot 0 at `+0x38`.

**Before** — `NSF813I BUSY=0 BUSYSLOT=-1 INFLIGHT=0 TMRQ=0 EXHAUSTED=0 COLLISIONS=0
REAPED=0`, `NSF812I ... SERVED=0`, slot 0 all zeroes.

**After:**

| field | value | reading |
|---|---|---|
| eyecatcher | `NSFVANCR` | anchor intact |
| `version` | `3` | unmoved |
| `flags` | **`00000000`** | `ANCHOR_ACTIVE` **cleared by recovery** (#79) |
| `inflight` | **`00000001`** | **leaked** |
| `served` | **`00000006`** | exactly the six requests that completed |
| `reaped` | `0` | nothing reaped |
| `server_ecb_ptr` | **`00000000`** | **cleared by recovery** (#79) |
| `nslots` | `00000040` | 64 |
| slot0 `req_state` | **`00000001`** | **PENDING** — never advanced to DONE |
| slot0 `reply_ecb` | **`809DE5F0`** | WAIT bit set — **client parked, never posted** |
| slot0 `xlen` | **`00000200`** | **512** — the staged count |

`served = 6` is exact: INITAPI, SOCKET, BIND, the `R0` trigger, the zero-byte receive,
the `R1` trigger. The seventh request — the data receive — never completed.

**`xlen = 512` settles the `ulen` question.** `nsfreqx_dispatch_in` sets
`priv->ulen = slot->xlen`, so the dispatcher saw 512, and `want = min(256, 512) = 256`.
The store genuinely ran with `n > 0`; a clean completion could not have been a silent
zero-length no-op, and the arm is not a disguised second copy of the control.

**The dangling shape differs from the write-out fault.** M5-2b1 measured a write-out
fault leaving `req_state` at **DONE** (slot busy forever). Here the fault is in the
*dispatch*, before any reply, so the slot stays **PENDING** and the client is parked on a
`reply_ecb` nobody will ever POST. The instrument was validated against independently
known values before it was trusted: the before-reading's `served`, `inflight`, `nslots`
and `server_ecb_ptr` all matched `NSF813I` and `NSF041I` exactly.

**The client hangs, and SYSPRINT is lost.** Both jobs had to be cancelled and both took
`S222`; the mbt matrix for run 1 read `FAIL ABEND S222` with **`0 PASS, 0 FAIL`** —
the buffered SYSPRINT went with the cancel. Every step is therefore marked with `wtof`,
and the console markers are the entire result.

---

## 4. Recovery — #79 validated in the field, twice, on a natural abend

#79's own gate used an **injected** failure. This round produced two real ones: the
executive taking an `S0C4` in the socket layer, not a contrived branch.

Both times recovery ran to completion — `NSF900E` → `NSF902I RECOVERY ENVIRONMENT:
SUP=N AUTH=Y` → `NSF903I` → `NSF901I` — and both times:

| | after run 1 | after run 2 |
|---|---|---|
| `S NSFS` on the same IPL | **succeeded** (STC 1593) | **succeeded** (STC 1594) |
| SVC steal | `NSF042I SVC 239 STOLEN (EP 00A8B248)` | `NSF042I SVC 239 STOLEN (EP 00AAD248)` |
| new anchor | `00AAD7C8` (was `00A8B7C8`) | `00ACF7C8` |
| `LARGEST FREE BLOCK NOW` | 1073152 → **933888** | 933888 → **794624** |

A different anchor **and** a different router EP each time is the evidence the module was
retained. Before #79 each of these abends cost an IPL; here they cost a restart.

**`NSF902I ... SUP=N AUTH=Y`** now has two more observations, on a fault nobody arranged.

**#83's `A0A` did NOT fire**, either time, with devices up (`NSF210I`/`NSF211I` on both
instances) — `NSF901I` was reached and the address space ended cleanly. The grep was
positive-controlled before this was believed: same command shape, `IEF450I` count 1 (the
NSFS abend), `A0A` count **0**, over a 524-line log that begins at IPL. This corroborates
the m5-79 record's reading that devices-up is **necessary but not sufficient** for #83 —
40-CHK's STC 1505 also abended with devices up and reached `NSF901I`. It is a free data
point about #83, not a finding about it, and nothing here was designed to test it.

---

## 5. The predictions, quoted as written

> **K(i) — the receive faults S0C4 in the executive.** The chain holds, the path is
> unusable, and the fix is a design question with ADR weight […] **Not this round's to
> build.**

**K(i) fires.** `IEF450I NSFS NSFS - ABEND S0C4 U0000`, twice, with the zero-byte
control completing `n=0` on the identical path in both runs.

> **K(ii) — it completes cleanly.** Then something protects it that the source read
> missed, and **naming that thing is the result**.

Not supported. Nothing protects it. (The contingency check — b0's `ISK` reading on the
`SP=241` block — was not needed, because the fault is the positive outcome; the anchor's
allocation key is only in question when the store *succeeds*.)

> **K(iii) — it fails some other way** (no data returned, the request never completes, an
> abend elsewhere).

Not supported, and the control is what excludes it. A bad pointer or a broken request
would have failed the zero-byte receive too; it returned `n=0`. The peer log proves the
datagram was sent, `xlen = 512` proves the length crossed, and `served = 6` proves the
six preceding requests round-tripped.

---

## 6. The probe is opt-in, on purpose

`TSTRQXR` abends NSFS and its receives are blocking with no timeout, so without the peer
it hangs holding an initiator and TESTLIB. A bare run therefore **does nothing at all**
and returns **CC 20** — the `XR_CC_GATE_SKIPPED` idiom already used by `TSTXFW` and
`TSTRQXF`, so "did not run" can never be read as "passed" (CLAUDE.md §8.5). The arm needs
`PARM='ARM'` via `jcl/TSTRQXR.jcl`.

**CC 20 alone would not have shown the guard works for the intended reason** — it is
also what you would see if `argc` never arrived under `crt1`, or if the PARM arrived in a
shape the compare misses. All three look identical from outside, which is §8.5 aimed at
the guard rather than at the code it guards. So the not-run branch reports what it saw,
and it was measured (JOB02868, batch and TSO):

```
11.01.27 JOB 2868  +TSTRQXR: NOT RUN -- argc=1 argv1=<none> (need PARM='ARM')
```

`argc=1` is the program name alone, so the PARM genuinely was absent and `argc`/`argv`
do arrive under `crt1`. The other direction is proven by JOB02864 having executed the arm
at all. The guard is therefore measured **both** ways, and independently corroborated from
the STC side: the instance the default run did not touch reported `NSF812I ... SERVED=0`
afterwards.

**Which binary carried which measurement.** The arm (§3) ran on the binary before this
marker was added; the marker run is the binary after. The change is **8 inserted lines
entirely inside the not-run branch** (`git diff --stat test/mvs/tstrqxr.c` → `1 file
changed, 8 insertions(+)`), so the arm's code path is untouched — the same standard
M5-2c0 used when it proved the RQE path unchanged by confining the diff. The match is
`strncmp(argv[1], "ARM", 3)`, a prefix test rather than an exact one; literal-to-literal,
so charset-transparent by the project's own rule.

---

## 7. What this does NOT establish

- **The inline (rxq-dequeue) shape was not driven live.** It reaches the identical
  instruction — `udp_complete_recv` is documented in its own header as shared by both
  paths — but that is an argument from source, not a measurement, and the call stack
  differs.
- **TCP was not driven live.** `tcp_recv_drain_to` reaches the same `buf_copyout` with
  the same `r->ubuf`, and `nsfreqx_dispatch_in` rewrites `ubuf` for *any* crossing request
  regardless of protocol, so it is the same store by construction. Still: measured on UDP,
  reasoned for TCP. This matters, because TCP is what M6's HTTPD and mvsMF would use.
- **Whether the faulting `memcpy` suppresses or terminates** is not pinned on this target,
  so a partial store is not excluded. Nothing here depends on it.
- **Any fix.** No key window was added, no landing area, no `ubuf` rewrite, and no
  existing key-0 window was widened to make anything pass. Red lines held.
- **Recovery from this fault.** The dangling state is *reported* (§3) and is unrecoverable
  from outside: `slot_action` is `ACT_NONE` for anything not PENDING-and-actionable, the
  anchor is INACTIVE, and the STC that owned it is gone. Write-out fault recovery remains
  the open M5-2 item ADR-0039 names.
- **#64, #83, c1 stage b** — untouched.

---

## 8. Cost and housekeeping

Each arm abends NSFS and retains its CSA anchor plus its SVC router module until IPL:
**139264 bytes, measured identically both times** (1073152 → 933888 → 794624). Two arms
cost 278528 bytes. `NSF055I ... LARGEST FREE BLOCK NOW` at each `S NSFS` is the free
running total.

Both probe jobs were cancelled and reached OUTPUT (`JOB02859`, `JOB02864`, both
`ABEND S222` — the cancel, not a defect). **Zero dumps across the whole round**, checked
rather than assumed: `IEA995I` count **0** in the console log against an `IEF450I` count
of **4** as the positive control — which is itself the right number, the two NSFS `S0C4`s
plus the two client `S222`s, with `IEF450I NSFS` exactly **2**. `JOB02864`'s SYSUDUMP
spool file is present and **0 bytes**. `SVC 239` was stolen and restored across every
start and stop. Host suite **2991 PASS / 0 FAIL**, unchanged — a no-regression
check only, since `src/nsfsx.c` and the probe are MVS-only and nothing host-buildable
changed.
