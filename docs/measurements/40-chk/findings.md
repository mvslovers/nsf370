# 40-CHK -- what the run established, and where it stopped

**Stand:** MVSCE on Hercules (`mvsdev`). NSFS STC 1505 (module as deployed for
PR #78), client `TSTAPPD` PARK arm, unauthorised. Times UTC unless a console
line is quoted (those are MVS local, ~1 h behind).

**Status: BLOCKED, and the blocker is finding 4.** The guard's decision was
measured; whether the POST proceeds was not. NSFS cannot currently be started
on this stand.

---

## 0. A correction to the round's premise, made from source before the run

The kickoff: *"the reply ECB lives in the client's private storage, and job
termination frees the region."* **It does not.** `asm/nsfvsvc.asm:579-585`
waits on `SLRECB` -- `NSFV_SLOT.reply_ecb` at slot+8, inside the CSA anchor
(`getmain(..., 241)`, key 0). Common storage; job termination does not free
it. The client may WAIT on a key-0 ECB because it is in supervisor state key 0
inside the SVC routine at that point (ADR-0038).

Confirmed live: the recorded ECB address is `00A8B808`, while the same job
reported its private storage as `STACK=000D1348 HEAP=00094C70`. **§2.3's
overlap question is answered negatively by the ECB's location** -- so §2.3 is
discharged rather than fulfilled, and the "POST into reused private storage"
story the kickoff anticipated does not arise.

A second mechanism, also from source: a stale POSTED bit cannot false-wake a
LATER client of the same slot. All four staging blocks zero the reply ECB
before publishing `req_state = PENDING` (`asm/nsfvsvc.asm` 371, 414, 440, 503),
for exactly that stated reason (line 359).

## 1. MEASURED: the guard answers LIVE for a client that died mid-request

The PARK arm bound port 7799 and blocked in RECVFROM, then was CANCELled.

**The request was outstanding at the moment of death** -- the discriminating
CONJUNCTION, not `PENDING` alone (which is also the state of a request already
in service):

```
[07:51:14Z] SLOT 0 @00A8B800  req_state=1 (PENDING) req_token=00000000
            reply_ecb=809DE6E0 req_ascb=00FE7B58 req_asid=0006 xfunc=6 xlen=64
NSF813I BUSY=1 BUSYSLOT=0 INFLIGHT=1 ...
```

`reply_ecb=809DE6E0` is a WAIT bit plus an RB address -- the client was
genuinely waiting on it.

**The client died:** `C TSTAPPDP` at 07:51:28Z -> JOB02812 `ABEND S222`,
status `OUTPUT`, gone from `D A,L`. It terminated cleanly; the recovery ladder
was not needed and the initiator was not wedged.

**The guard's own arithmetic, on that exact pair, after the death:**

```
NSF815I   SLOT  2 TOKEN=00010002 ASCB=00FE7B58 ASID=0006 LIVE
```

So **G(i)'s first half is measured**: for a batch client that died with a
request outstanding, the ADR-0040 guard answers **LIVE**. This is the stage-a
mechanism (`docs/measurements/m5-2c1/stage-a.md`) reaching the guard: the ASCB
is the initiator's, the initiator did not end, the ASVT entry never went
AVAILABLE.

## 2. NOT MEASURED: whether the POST proceeds

Completing the parked request took **NSFS down with `S0C4`** before the guard's
decision was acted on.

```
2.52.15 STC 1505  NSF900E NSF ABEND INTERCEPTED -- CAPTURING AND PERCOLATING
2.52.15 STC 1505  NSF901I NSF EMERGENCY TEARDOWN COMPLETE
2.52.15 STC 1505  IEF450I NSFS NSFS - ABEND S0C4 U0000
```

**The fault is BEFORE the classify decision, and the anchor proves it.** In
`nsfsx_drain` step 1 the LIVE branch is `served++` -> `req_state = DONE` ->
`__xmpost`. The CSA anchor survives the abend and reads, afterwards:

```
[07:55:50Z] ANCHOR ... inflight=1 served=7 ...
            SLOT 0 ... req_state=1 (PENDING) ...
```

`served` unmoved at its pre-datagram value of 7, `req_state` still PENDING. So
neither `served++` nor the state change ran, and the POST was never reached.

**Leading hypothesis, NOT confirmed** -- the control that would confirm it did
not run (finding 5). `src/nsfudp.c:200` completes an RQ_RECVFROM with
`buf_copyout(bpay, r->ubuf, want)`. In Phase 2 `nsfreqx_dispatch_in` rewrites
`ubuf` to `slot->stage`, inside the key-0 `SP=241` anchor, and the executive
dispatches OUTSIDE the key window, in key 8. M5-2b0 measured precisely that
store faulting `S0C4` on an `SP=241` block. Every other CSA write in this
design sits inside a key window (`RQEIN`/`RQEOUT`/`MOVEOUT`, and
`nsfreqx_result_out` under `__super`); the protocol op's `ubuf` write is the
one that does not.

**Bounded:** no test in this tree exercises a cross-AS data-returning receive
-- TSTRQXM covers `send` (which READS `ubuf`), `connect` and `close`.
**Phase 1 is unaffected**, which is why TSTUDP/TSTTCP are green: there `ubuf`
is the app's own same-space key-8 pointer, no CSA and no fault.

**It has nothing to do with the client being dead**, which is why it is a
confound rather than the answer. Not fixed here: the round changes no
production logic, and the fix touches the same `SPKA` ground M5-2b1/c0 spent
two rounds on. No issue opened (CLAUDE.md 8 rule 8).

## 3. The pivot that was prepared and not run

`accept` and `connect` never touch `ubuf` (the only uses in `src/nsftcp.c` are
TCP send, which reads, and TCP recv, which writes), and `soc_complete` writes
its results into the STC-PRIVATE copy, which `nsfreqx_result_out` then copies
to CSA inside the key window. So a parked ACCEPT completed by a host connect
reaches the guard and the POST with nothing CSA-touching running in key 8.
That arm is written (`PARK`'s sibling) but blocked by finding 4.

## 4. **BLOCKING, and a violation of a stated invariant: an abend leaves SVC 239 stolen**

After the `S0C4`, `S NSFS` fails and the address space abends:

```
2.59.10 STC 1506  NSF049E SVC 239 IN USE (EP 00A820C8) -- NOT STOLEN
2.59.10 STC 1506  NSF009E NSFS TRANSPORT INITIALIZATION FAILED
2.59.10 STC 1506  IEF450I NSFS NSFS - ABEND SA0A U0000
```

`00A820C8` is STC 1505's own router EP (`NSF042I SVC 239 STOLEN (EP
00A820C8)`). The orderly path restores the slot (`NSF043I SVC 239 RESTORED`,
seen at every clean `P NSFS`); **the recovery path does not.**

From source: `nsf_recover` (`src/nsfsmain.c:88`) calls `nsf_shutdown()` and
nothing else. `nsfsx_stop()` -- which restores the SVC slot, invalidates the
published ECB address, drains and unloads the router -- is called only from
`nsfsmain.c:452`, the orderly path.

**This contradicts an invariant stated in two places.** CLAUDE.md 3: *"The
recovery path calls the same destroy/quiesce functions as the orderly path. A
crash must never require a Hercules restart to clean up."* And `nsf_recover`'s
own comment: *"no Hercules restart is ever needed to clean up (goal,
ADR-0006)."* For the Phase-2 STC that goal is not met.

**The refusal is correct and is not the defect.** `nsfsx_start` steals slot 239
only if its EP still equals the modal "unused" filler EP, so it declines rather
than clobber a slot somebody else owns. There is no takeover path by design,
and there should not be.

**State left behind, read from CSA:** the old anchor at `00A8B7C8` is intact
and still flagged ACTIVE (`flags=80000000`), with `inflight=1`, slot 0 still
PENDING, and `server_ascb=00FF8B20` pointing at the terminated STC 1505. The
router module was never unloaded, so SVC 239 still reaches valid code -- but
that code would publish into the orphaned anchor and cross-AS POST into a dead
address space's private storage. **No NSF client may be run until this is
cleared** (the mirrored STC-death race ADR-0041 records as residual risk,
reached here by an ordinary abend).

**Cost per abend:** the CSA pool plus the router leak. `NSF055I ... LARGEST
FREE BLOCK NOW` went 1073152 (STC 1505) -> 933888 (STC 1506), ~139 KB.

## 5. A ROUND-HYGIENE FAILURE OF MINE, and it is the project's own trap

I sent the C1 datagram and read `IEE341I NSFS NOT ACTIVE`, and my first
reading was "the datagram crashed it with no client attached". **It had not
run at all** -- NSFS abended `SA0A` at 2.59.10, thirty-six seconds *before* the
datagram, for the reason in finding 4. Only reading the console log rather
than the command's result separated them.

An absence that looks exactly like its own result, which is CLAUDE.md 8 rule 5
in its purest form. The lesson for the next arm: **a control that never ran and
a control that ran and failed are two states, and `NOT ACTIVE` distinguishes
neither.** Start-up must be positively confirmed (`NSF041I`/`NSF001I`) before
any stimulus is sent.

## 6. The three predictions

* **G(i)** -- *"the guard answers LIVE and the POST proceeds"*. **First half
  measured** (finding 1). Second half **not reached** (finding 2).
* **G(ii)** -- *"something else stops the POST"*. Not supported as written. An
  `S0C4` did stop it, but it is a crash and not a check, and it is upstream of
  the guard rather than a partner to it.
* **G(iii)** -- *"the client's death releases the slot"*. **Refuted.** The slot
  stayed `PENDING` with `inflight=1` across the death and is still so.

The run therefore supports **G(i) as far as it goes and no further**, and adds
two things none of the three anticipated: the `ubuf` key-0 write (finding 2)
and the recovery-path SVC leak (finding 4).
