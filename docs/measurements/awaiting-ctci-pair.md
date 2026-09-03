# Properties awaiting a working CTCI pair

**One item, deliberately, and it is not in a round folder.** Two rounds
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

---

## 1. d1 §2.3 -- the SELECT arms' stimulus

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

## 2. Job A §1.3 -- a protocol op reading `g_land`

`docs/measurements/m5-2e-joba/README.md`.

The bulk verb is a non-blocking `RECVFROM` on an empty rxq, answered
`EWOULDBLOCK` before `udp_recv` touches `ubuf`, so what the round proves is the
**transport's copy pair** -- in full, both directions, 428 822 calls with two
clients alternating and `dirty=0`. The once-per-checkpoint `sendto` is the only
case in which a protocol op would actually **read** the landing area, and it
**did not run** (`wire=0 ... *** NO INTERFACE -- WIRE ARM DID NOT RUN ***`,
asserted in positive form, corroborated by `LNK1 oerr 2`).

What is needed: the same gate, unchanged, on a stand whose interface is up. The
arm is already written and already asserts fully when the interface is present.

**There is no wire-free substitute, and this was checked rather than assumed:**
`src/nsfhost.c` -- the loopback/TUN driver that would otherwise serve -- is a
**host-build replacement only**. `project.toml` maps `src/nsfhost_plat.c` ->
`src/nsfhost.c` for the host build, and on MVS `nsfhost_plat.c` is a NULL-ops
placeholder. On the stand, CTCI 0500/0501 is the only path to a device.

**Owner:** whoever next runs Job A with the pair up. It costs one re-run of an
unchanged gate, not a change to it.

---

## Neither of these is a reason to delay anything

Both properties are **additional confirmation of conclusions that already
stand on other evidence** -- d1's ownership claim on 2.2/2.2b, Job A's §1.3 on
the transport copy pair. They are recorded so that "we meant to run that" does
not quietly become "we ran that".
