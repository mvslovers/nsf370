# Properties awaiting a working CTCI pair

**One file, deliberately, and it is not in a round folder.** Two rounds
independently reached a property they could not exercise because `tun0` was
down, and each recorded it as a footnote in its own README. A precondition
split across two footnotes in two files does not come back -- so it is
collected here, and each record carries a **one-line pointer** to this file
rather than a restatement.

**Membership criterion, so a later round can add to this correctly:** a
property whose assertions are **already written and already correct**, and
whose *only* missing precondition is a working CTCI pair on the stand. Not
"things that would be nice to test", and not anything blocked on a design
decision, an unfixed defect or a missing instrument.

**This is NOT a list of what is broken about the emulator.** The reason the
pair is down is a finding about the driving system and lives separately, in
`docs/measurements/ctci-tun-eintr/` -- keeping them apart is what stops "the
wire is broken" being read as an excuse for either. Fixing the wire does not
discharge anything below; it only makes the runs possible.

**Discharged entries are kept, marked, and not deleted.** A record elsewhere
that points at "item 2" must still find something when it gets here, and an
entry that vanishes on being closed reads afterwards as one that was never
written.

---

## OPEN

### 1. d1 §2.3 -- the SELECT arms' stimulus

`docs/measurements/m5-2-d1-select/README.md`, annotation 2026-09-03.

Arms 1 and 2 (`TSTD1B` roles A + B) have **no confirmed stimulus** on record:
nothing shows the host connect was made, and nothing shows A ever became
read-ready, so `rc=0 ready=0` is equally consistent with *"A never became
ready"*. The poll arm has the same gap -- `tcp_poll` makes a listener
READ-ready only on a non-empty `rxq`/`acceptq` or `TCB_F_RCVFIN`, so
`foreign.ready == 0` is also what a *resolved, idle* listener yields. **Neither
arm separates "refused" from "resolved and idle".**

What is needed: re-run with the connect confirmed on the wire, the way arm 3's
`Ncat: Connected to 192.168.200.1:3013` confirms its own.

**Not weakened, and not to be re-litigated here:** the round's crossing-level
ownership conclusion rests on 2.2 / 2.2b, which carry their own positive
control and need no connect. Arm 3 is unaffected -- its stimulus is confirmed.

**Owner:** a d1 round. #107 assigns it there, and **(e) does not cover it**.

**Still open after 2026-09-15**, when the pair was up and item 2 below was run:
that round drove Job A's gate only and deliberately did not touch these arms.
A working wire is now a demonstrated condition on this stand rather than a
hope, so what remains is scheduling the d1 round, not waiting for anything.

---

## DISCHARGED

### 2. Job A §1.3 -- a protocol op reading `g_land` -- **CLOSED 2026-09-15**

**Run and green:** `docs/measurements/m5-2e-joba-wire/`.

The gap was that Job A's bulk verb is a non-blocking `RECVFROM` on an empty
rxq, answered `EWOULDBLOCK` before `udp_recv` touches `ubuf`, so that round
proved the **transport's copy pair** in full -- both directions, 428 822 calls,
two clients alternating, `dirty=0` -- and nothing about a protocol op actually
**reading** the landing area. The once-per-checkpoint `sendto` is that case,
and it had not run (`wire=0 ... *** NO INTERFACE -- WIRE ARM DID NOT RUN ***`).

It cost exactly what this entry predicted: **one re-run of an unchanged gate**,
no source change. `wire=7 ok=7` and `dirty=0` on both clients, **A 19/19 and B
24/24, CC 0000**, corroborated independently by 14 × 1024-byte datagrams in the
host's own `tcpdump` and by `NSFUDP out 14` / `LNK1 out 16` on the STC.

*Kept rather than deleted* because `m5-2e-joba/README.md` names "item 2" in its
body text, and because the entry is the worked example of the membership
criterion above: written assertions, one missing precondition, one re-run.

---

## Neither of these was a reason to delay anything

Both properties are **additional confirmation of conclusions that already
stand on other evidence** -- d1's ownership claim on 2.2/2.2b, Job A's §1.3 on
the transport copy pair. They are recorded so that "we meant to run that" does
not quietly become "we ran that" -- which is also why item 2's closure names
the round that ran it rather than simply disappearing.
