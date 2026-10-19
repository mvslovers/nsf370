# 40-CHK -- predictions and recovery ladder, recorded BEFORE the induction ran

## The three predictions, quoted verbatim from the kickoff

> **G(i)** — the guard answers LIVE and the POST proceeds. The ADR-0040
> protection is inert for batch clients, which is every client in this tree
> today. ADR-0040 gets an annotation naming the class it does not cover, and
> the question of what to do about it is Mike's.

> **G(ii)** — something else stops the POST — the slot state, the eyecatcher,
> a check nobody has named in this thread. Then the guard has a partner that
> does the real work, and that partner should be named in ADR-0040 rather than
> left implicit.

> **G(iii)** — the client's death releases the slot before the STC gets there,
> so no POST is ever attempted. Then the hole is unreachable by this route and
> the question narrows to whether another route reaches it.

> If the readings fit none of the three, report them raw.

## A correction to the round's premise, made BEFORE the run

The kickoff states: *"the reply ECB lives in the client's private storage, and
job termination frees the region."*

**It does not.** `asm/nsfvsvc.asm:579-585`:

```
*  WAIT for the reply on the key-0 CSA reply ECB, supervisor state, key 0.
         LA    R3,SLRECB(,R7)     A(our slot's reply_ecb)
         WAIT  1,ECB=(R3)
```

`SLRECB` is `NSFV_SLOT.reply_ecb` at slot+8 -- inside the CSA anchor
(`SP=241`, key 0), which is common storage and is **not** freed by job
termination. The client can WAIT on a key-0 ECB because it is in supervisor
state key 0 inside the SVC routine at that point (ADR-0038).

This does not touch the guard question, which is measured either way. It
changes what §2.3 can find: the overlap it asks about is answered by the
ECB's *location*, so §2.3 is discharged rather than fulfilled.

## A second mechanism, checked from source before the run

A stale POSTED bit cannot false-wake a LATER client of the same slot. All four
staging blocks zero the reply ECB before publishing `req_state = PENDING`
(`asm/nsfvsvc.asm` lines 371, 414, 440, 503 -- ECHO, XFER, ORPHAN, RQE), with
the reason stated at line 359: *"zero the reply ECB first so a stale post
cannot false-wake the WAIT."*

So if G(i) fires, the predicted damage is a **resource leak**, not corruption:
`req_state` stuck at DONE and `inflight` never given back -- the OUT-direction
dangling shape M5-2b1 measured. That is a materially better answer than the
kickoff anticipated, and it is predicted here so a confirmation is a
measurement.

## What "outstanding" requires, and why PENDING alone will not do

`req_state == PENDING` is ALSO the state of a request already taken into
service: `nsfsx_any_pending_other` skips the in-service slot precisely because
it stays PENDING until the drain finishes it. So the discriminating reading is
the CONJUNCTION, taken at one moment:

  slot `req_state == 1` **and** `NSF813I BUSY=1 BUSYSLOT=<that slot>`

## Recovery ladder for the induction (the risk this round actually runs)

The client is cancelled while in supervisor state, key 0, inside the SVC
routine's `WAIT`, holding a CSA slot. If RTM cannot terminate it, the
initiator is wedged.

    C jobname  ->  C jobname,DUMP  ->  FORCE  ->  IPL

**Termination is VERIFIED before anything else proceeds** (`D A,L` shows the
job gone AND the jobid reaches OUTPUT). If it stays ACTIVE, the round stops
and reports that: a non-cancellable client parked in the transport is a
finding in its own right and escalating past `C ...,DUMP` is not this round's
call to make.

`nsfsx_stop`'s nudge loop is NOT a recovery path here -- it POSTs parked
clients, and this client is dead.

---

# Round 2 -- recorded after the first induction crashed the STC, before the next arm

The first induction reached the guard's *precondition* but not the guard. The
completing datagram took NSFS down with **`S0C4`**, and the anchor -- which
survives the abend -- shows `served` UNMOVED at 7 with `req_state` still
`PENDING`. In `nsfsx_drain` step 1 the LIVE branch is
`served++` -> `req_state = DONE` -> `__xmpost`, so **the fault is before the
classify decision**. The guard question is untouched and still open.

## The standing discriminator, for every crash from here

The CSA anchor survives the abend, so after any S0C4 read two fields:

* `served` unmoved **and** `req_state == 1 (PENDING)` -> fault BEFORE the
  classify decision.
* `served` +1 **and** `req_state == 2 (DONE)` -> fault in or after `__xmpost`,
  i.e. the POST path -- which is the thing this round is about.

## C1 -- exonerate the device path, with NO client at all

`LNK1 in 0` at the reading before the crash: that datagram was the **first
inbound frame this instance had ever seen**. So "any inbound frame S0C4s this
build" is a live alternative, and a live-client control cannot separate it from
the `ubuf` store -- both fire in both arms.

Send one datagram to a port with **nothing bound**. It drives CTCI read ->
codec -> `nsfip_input` -> `nsfudp_input` -> `noport` -> ICMP port-unreachable,
and never reaches `buf_copyout`.

**Prediction: it survives.** Counters move, ICMP port-unreachable on the wire,
STC alive. If it crashes instead, the finding is a device bug and this round's
framing changes completely.

## C2 -- the suspected mechanism, stated before it is tested further

`src/nsfudp.c:200` completes an RQ_RECVFROM with
`buf_copyout(bpay, r->ubuf, want)`. In Phase 2 `nsfreqx_dispatch_in` rewrites
`ubuf` to `slot->stage`, inside the anchor taken by `getmain(size, 241)` --
**SP 241, key 0** -- and the executive dispatches OUTSIDE the key window, in
key 8. M5-2b0 measured exactly that store faulting **`S0C4`** on an
`SP=241` block.

Every other CSA write in this design sits inside a key window
(`RQEIN`/`RQEOUT`/`MOVEOUT`, and `nsfreqx_result_out` under `__super`). The
protocol op's `ubuf` write is the one that does not.

**It has nothing to do with the client being dead** -- which is why it is a
confound and not the answer.

**Bounded claim:** no test in this tree exercises a cross-AS data-returning
receive. TSTRQXM covers `send`, `connect` and `close`. **Phase 1 is
unaffected**: there `ubuf` is the app's own same-space key-8 pointer, no CSA
and no fault, which is why TSTUDP/TSTTCP are green.

**Not fixed here.** The round changes no production logic, and the fix touches
the same `SPKA` ground M5-2b1/c0 spent two rounds on. Reported to Mike; no
issue opened (CLAUDE.md 8 rule 8).

## C3 -- the pivot: reach the guard by a `ubuf`-free verb

`accept` and `connect` never touch `ubuf` (`src/nsftcp.c` -- the only `ubuf`
uses are TCP send, which READS, and TCP recv, which writes). `soc_complete`
writes `retcode`/`p1`/`p2` into the STC-PRIVATE copy, and
`nsfreqx_result_out` copies private -> CSA inside the key window, so nothing
CSA-touching runs in key 8 on that path.

New arm: INITAPI, SOCKET(STREAM), BIND, LISTEN, blocking **ACCEPT** -> parked.
CANCEL the client. Then connect from the host, which completes the accept and
drives the STC to the guard and the reply POST.

**Prediction: the guard answers LIVE and the POST proceeds** -- G(i) -- and
the POST may itself `S0C4`, following the dead task's RB remnant left in
`reply_ecb` (`809DE6E0` in the first induction; a WAIT bit plus an RB address
belonging to a task that no longer exists). `served`/`req_state` discriminate:
+1 and DONE means the fault is at or after the POST.

That would be a stronger G(i) than a leak: the guard says LIVE, the POST goes
ahead, and it takes the STC down.

Fallback if accept misbehaves: `connect` to an unresponsive address on the
link (192.168.200.99:9999) parks until RTO give-up (~190 s), also `ubuf`-free.
