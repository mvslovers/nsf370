# Why 0500/0501 do not come up: an unretried EINTR in Hercules' hercifc handshake

**Date:** 2026-09-03 · **Stand:** mvsdev, Hercules 4.10.0.11739-SDL-DEV-g60dd927e
(built 2026-08-25), source at `~/hercules/hyperion` (the exact running source).
**Status:** root cause established by measurement. **Not fixed** -- the fix is a
change to Hercules, not to this repo, and it is Mike's call.

This is the failure `docs/measurements/m5-79/` recorded and could not chase
(it needed the spool it had just exhausted). It cost M5-2e Job A its wire arm.

---

## The symptom

At every Hercules start, in the same second, no retry, no delay:

```
HHC00138E Error setting TUN/TAP mode : Interrupted system call
HHC00007I Previous message from function 'TUNTAP_CreateInterface' at tuntap.c(458)
HHC01463E 0:0501 device initialization failed
HHC00007I Previous message from function 'attach_device' at config.c(1366)
```

`tun0` never exists, MVS IPLs without 0500/0501, and 3.8j has no dynamic I/O
reconfiguration -- so the pair is gone until the next Hercules start.

**It is NOT intermittent any more.** It was on 2026-08-30 (m5-79); it
reproduced on **both** starts today, including a deliberate clean IPL cycle
performed to test exactly that. Treating it as a race that "usually works" is
no longer correct.

## The chain, and how each link was established

| link | how established |
|---|---|
| `TUNSETIFF` from an unprivileged process returns **EPERM**, not EINTR | **MEASURED.** A 20-line reproducer run as `mike` on this box: `try 0/1/2: TUNSETIFF rc=-1 errno=1 (Operation not permitted)`. So the ioctl is NOT the source of the EINTR, and Hercules necessarily takes its EPERM fallback. |
| the fallback forks `hercifc` | source, `tuntap.c:108-118` -- `if (0 > rc && errno == EPERM && !(IFF_NO_HERCIFC & iFlags))` |
| `hercifc` is present, setuid root, and the same build | **MEASURED.** `-rwsr-xr-x 1 root root .../bin/hercifc`, filesystem is `rw,relatime` (**not** `nosuid`), and running it by hand prints its banner and processes a request: `HHC02499I ... version 4.10.0.11739-SDL-DEV-g60dd927e`, build date `Aug 25 2026`, identical to `hercules`. |
| `execlp("hercifc", ...)` can find it | **MEASURED.** `HERCIFC_CMD` is the bare string `"hercifc"` (`hercifc.h:14`), so `execlp` searches `PATH`; the **running** process's `PATH` (read from `/proc/<pid>/environ`) contains `/usr/local/hercules/bin`, and `hercifc` is there and executable. |
| the parent then waits: `select(ifd[1]+1, &selset, NULL, NULL, {5,0})` | source, `tuntap.c:156-160` |
| **that `select` returns -1 / EINTR, and nothing retries it** | source + the message. `tuntap.c:161-176` handles `rc > 0` (read the reply) and `rc == 0` (timeout -> `HHC00135`), and has **no branch for `rc < 0`** -- so `rc` stays -1, `errno` stays EINTR, `TUNTAP_SetMode` returns -1, and the caller prints `HHC00138E` with `strerror(errno)` = "Interrupted system call". That is exactly the observed message. |
| the signal arrived **immediately**, not after a wait | **MEASURED, from the log's own timestamps.** Every message is in the same second and `HHC00135` (the 5-second timeout) never appears, so `select` returned at once rather than waiting. |

**One inference is labelled as such: WHICH signal.** The code defect does not
depend on it. A plausible candidate is `SIGCHLD` from the forked `hercifc`, and
the log ordering is at least consistent with thread-startup signalling -- the
HTTP server thread is created in the two lines immediately before the failure
(`HHC01807I` / `HHC00100I ... 'http_server' started`). **Neither is
established**, and pinning it needs `strace`, which is not installed on this box
and cannot be installed without a password we do not have.

## Precedent: the same codebase already treats an interrupted wait as a retry

Recorded because it is precedent rather than opinion, and it is what makes the
missing retry look like an **oversight rather than a decision**. **Nothing was
changed.** Two sites in `ctc_ctci.c` -- the very driver that uses this tun:

- `ctc_ctci.c:776` -- `if (rc == ETIMEDOUT || rc == EINTR) continue;`. An
  explicit, named EINTR retry.
- `ctc_ctci.c:1075` -- `if( iLength == 0 ) continue;`, commented
  `(probably EINTR; ignore)`.

So an interrupted wait is treated as retryable twice over in the CTCI driver,
while the tun bring-up one layer down reports it as a device failure.

## A second, independent defect found on the way

**Hercules leaks the tun fd on this error path.** `TUNTAP_CreateInterface`
returns -1 without closing the fd it opened (`tuntap.c:424-459`). Measured on
the live process: `lsof /dev/net/tun` shows `panel_dis 620872 mike 37u CHR
10,200 ... /dev/net/tun` -- still open, long after the failure, and `lsmod`
shows `tun` with a use count of 2 while **no tun interface exists in any
namespace**. Harmless here (one fd per start) but it is why the module looks
busy when nothing is using it.

## What was ruled OUT, and by what

- **`hercifc` missing, not setuid, or on a `nosuid` mount** -- all three
  measured false.
- **A stale `tun0` or a leftover interface blocking the name** -- `ip -br link
  show type tun` is empty, `ip netns list` is empty. Hercules also asks the
  kernel to allocate the name, so a collision could not produce this anyway.
- **The kernel refusing TUNSETIFF with EINTR** -- the reproducer gets EPERM
  three times out of three.
- **A version skew between `hercules` and `hercifc`** -- identical build
  strings and identical mtimes (`25. Aug 19:50`).

## Fix options, in the order I would take them

1. **Retry the interruptible waits** in `TUNTAP_SetMode` (`~/hercules/hyperion/
   tuntap.c`): wrap the `select()` and the `read()` in `do { } while (rc < 0 &&
   errno == EINTR)`, and give `rc < 0` an explicit branch so an interrupted wait
   can never be reported as "device initialization failed". This is the correct
   fix, it is small, it is upstream-able, and the exact running source is on the
   box -- so it can be tested here by rebuilding and restarting. **Not done:
   rebuilding Mike's emulator is his call.**
2. **Bypass `hercifc` entirely** by pre-creating a persistent tun owned by
   `mike` and using Hercules' preconfigured-interface path (`IFF_NO_HERCIFC`;
   `HHC00154` is its error message). Removes the failing code path from the
   startup entirely. Needs root once to create the persistent device.
3. **Restart and hope** -- what we have been doing. **Today it did not work**,
   which is what makes 1 or 2 worth doing.

## Consequence already absorbed

M5-2e Job A was made **device-independent** because of this (its bulk verb is a
non-blocking `RECVFROM` rather than a `sendto`), so the exit gate now runs on a
stand with no CTCI pair and says which arms did not. See
`docs/measurements/m5-2e-joba/`.

The stack properties that are waiting on the pair are listed in
`docs/measurements/awaiting-ctci-pair.md`. **They are deliberately not part of
this document:** this one is a finding about the driving system, that one is a
list of stack properties awaiting a stimulus, and fixing the wire discharges
nothing on it -- it only makes the runs possible. Keeping them apart is what
stops "the wire is broken" from being read as an excuse for either.

---

# ADDENDUM 2026-09-14 — diagnosing the CHANGE, not the mechanism

**Appended, nothing above rewritten.** Everything above was measured and stands.
Read-only round: nothing stopped, started, rebuilt or reconfigured, no `sudo`.

**The question was: the missing `EINTR` retry has been in that source for years
and the pair carried real traffic on 2026-09-02, so what changed such that this
path is entered at all?** The kickoff's premise was that `EPERM` must itself be
a regression -- because if `TUNSETIFF` succeeded, `hercifc` would never be
forked, no signal could interrupt anything, and nothing would reach the broken
branch.

## THE PREMISE DOES NOT HOLD ON THIS BOX, and that is the round's answer

**`EPERM` is the DESIGNED path here, not an anomaly**, so it cannot be the
change:

- **No Hercules binary carries any capability** -- all three executing binaries
  report `(none)`. *Controls:* `/usr/bin/tcpdump` reports
  `cap_net_admin,cap_net_raw=eip`, so `getcap` works and an empty result is a
  real "none"; a nonexistent path prints `(No such file or directory)`, so a
  failure cannot read as "none". **And `getcap` was not on the non-interactive
  PATH on the first attempt** -- without the control that would have read as
  "no capabilities" when the tool had not run at all.
- **`hercifc` IS setuid root** -- `-rwsr-xr-x 1 root root ... 6. Sep 11:03`.
  *Control:* `/usr/bin/passwd` and `/usr/bin/sudo` show the same `s`, so the bit
  is visible when present.

Those two facts together are the point: **this box uses the setuid-`hercifc`
arrangement, and that arrangement REQUIRES the direct ioctl to fail `EPERM`**
(`tuntap.c:108` forks the child only `if (... errno == EPERM ...)`). A
capability on `hercules` would make `hercifc`'s setuid bit pointless. The bit
was moreover **re-set after the 6 September rebuild**, so it is actively
maintained. `EPERM` was the path on 2 September when CTCI worked.

**Consequence for the decision this round exists to inform:** the worry that a
`tuntap.c` patch would paper over a configuration regression **does not apply**
-- there is no configuration regression in the `EPERM`. That does not make the
patch right; it removes one argument against it. The decision stays Mike's.

## Refuted, each with its evidence

| hypothesis | verdict |
|---|---|
| a lost `cap_net_admin` on `hercules` | **refuted as the change** -- none present, and the setuid-`hercifc` design means none was needed |
| the rebuild dropped `hercifc`'s setuid bit (the failure this project has seen before, 2026-08-21) | **refuted** -- the bit is set |
| the `nohif` line means a local source hack | **refuted** -- the tree is CLEAN and `nohif` is committed, in `4d171e51` "Add utun support for CTCI and QETH layer 3 on macOS" |
| that utun commit changed the Linux path | **refuted by date** -- it landed **2026-08-15**, before the last known-good run, and is in the build that worked |
| the Hercules rebuild caused it | **refuted by date** -- the rebuild is **6 Sep**, the first failure **3 Sep**, and the failure spans BOTH builds (`11739-g60dd927e` and `11774-g59d8981c`) |
| a kernel or boot-environment change | **refuted** -- `uptime` is **36 days** (up since 9 Aug), continuously across both the working and failing periods, so the running kernel never changed |
| the device node or group membership | **no route** -- `/dev/net/tun` is `crw-rw-rw-` so the open succeeds; `TUNSETIFF` needs `CAP_NET_ADMIN`, which `netdev`/`sudo` membership does not grant |
| **`SIGCHLD` from the forked child** -- the candidate THIS DOCUMENT floated above | **REFUTED.** `SigCgt` for the live process is `0x1000044cb`: SIGHUP, SIGINT, SIGILL, SIGBUS, SIGFPE, SIGSEGV, SIGTERM and one real-time signal. **Bit 17 is clear -- SIGCHLD is not caught**, so a child exiting does not interrupt `select()`. The §mechanism text above labelled it "a plausible candidate ... neither is established"; it is now refuted, and the label is why that costs nothing. |

## The stand is not the stand that worked, and that reframes everything

- **`~/MVSCE` no longer exists.** It was replaced on **4 September** (there is a
  `MVSCE.release.v3.0.0.tar` of that date) by `MVSCE-EXP`, `MVSCE-DEV` and later
  `MVSCE-LAB`. **Five** Hercules instances now run on this box, on two different
  binaries.
- **Only `MVSCE-DEV` configures CTCI at all** -- `0500,0501 CTCI 192.168.200.1
  192.168.200.2`, the line this document quotes. The other four stands have no
  CTCI or LCS line, so they are not competing for the interface.
- **`MVSCE-DEV` has started exactly ONCE and CTCI failed on that one start**
  (one `HHC01413I`, one `HHC00138E`/`HHC01463E` pair). **The pair has never
  worked on this stand.**
- **No surviving host-side log records a successful CTCI attach.** *Control:*
  the three logs exist, are non-empty, and `HHC01413I` is found in all of them,
  so the empty result is a real absence and not an unreachable path. The
  working period's logs went with `~/MVSCE`.

## What this does NOT establish, stated plainly

**The ladder does not explain what changed, and I am not going to fit a story
to it.** Same kernel, same uptime, `hercifc` healthy and setuid, no capability
arrangement in use, and the failure spans two Hercules builds and a complete
stand replacement. What is left is that the `hercifc` handshake is interrupted
by *some* signal, and:

- **WHICH signal is not established.** SIGCHLD is now refuted. A real-time
  signal is caught (`SigCgt` bit 32), which is consistent with a periodic timer,
  but *consistent with* is not evidence and no further weight is put on it.
- **WHEN and HOW the change happened is not established**, nor **who made it**,
  nor **whether anything else was affected by the same event**. There is no
  host-side artifact from the working period left to compare against.
- **`/var/log/dpkg.log` could not be checked.** Its positive control FAILED --
  zero entries in the date range -- so the empty result is uninterpretable and
  is reported as "not checked", not as "nothing found".
- Settling the signal needs `strace`, which is **not installed**; installing it
  needs `sudo`, which needs a password we do not have. **That is the one check
  that would close this, and it is the reason the round stops here.**

## The `EINTR` fall-through is still a real defect

Independently of all the above, and unchanged: `TUNTAP_SetMode` has no `rc < 0`
branch, so an interrupted wait is reported as "device initialization failed",
and the same codebase treats an interrupted wait as retryable twice over in
`ctc_ctci.c` (see §Precedent). **But on the evidence here it is not merely a
downstream consequence of an anomalous `EPERM` -- the `EPERM` is by design, so
the fall-through is the PROXIMATE cause of the attach failing**, and the open
question is what delivers the signal. That is a correction to the kickoff's
framing, not to the mechanism analysis above, which stands as written.

## Proposal only -- NOT run, and not a recommendation

If the capability route were ever chosen over the setuid-`hercifc` route, the
command would be `sudo setcap cap_net_admin+ep /usr/local/hercules/bin/hercules`.
**It was not run.** It would also **diverge this stand from the stock Hercules
that TK4-/TK5 users run**, which is the configuration NSF must work on, and it
would mask rather than fix the fall-through. Recorded so the option is visible,
not because it is advised.

---

## CORRECTION 2026-09-14 — the stand was RENAMED, not replaced

**Appended; the addendum above is not rewritten and the refutations in it are
untouched.** Read-only round, nothing changed on the box.

### The false fact, and what replaces it

From the addendum's context section, verbatim:

> **`MVSCE-DEV` has started exactly ONCE and CTCI failed on that one start**
> (one `HHC01413I`, one `HHC00138E`/`HHC01463E` pair). **The pair has never
> worked on this stand.**

**Superseded. `~/MVSCE` was RENAMED to `~/MVSCE-DEV`** (Mike): same 2.1.4
installation, same configuration, same files. **The CTCI pair worked on exactly
this instance on 2 September.** The `MVSCE.release.v3.0.0.tar` of 4 September is
a separate thing on the same box and is not this instance.

Corroborated here rather than taken on trust: `~/MVSCE-DEV/SCRIPTS/` holds
**`pjes2 poweroff quiesce SHUTDOWN.RC zeod`** -- the same five files, by name,
that `~/MVSCE/SCRIPTS/` held when this document's first round listed them on 3
September; the oldest content carries **2026-07-08** mtimes, months before the
4-September tar; and the directory's own mtime/ctime is **2026-09-04 18:49:05**,
which is the rename.

### The mechanism of the error -- the same shape as the premise error, one layer in

**`hercules -o hercules.log` TRUNCATES the log at every start**, so a count of
`HHC01413I` banners can only ever be **1**. Measured across all three MVSCE
logs: each is 425 KB / 972 KB / 1.1 MB, each **begins** with a banner, and each
contains **exactly one**. "Started exactly once" was never a property of the
instance -- it is a property of the logging, and every stand on the box reports
it.

**The control I ran could not have caught this.** It showed *"the logs exist and
`HHC01413I` is found in all of them"* -- that the search ran and the files were
reachable. **It could not show what period the logs cover**, and coverage is
what the figure depended on. The control that was needed is the one now run:
each log's first line, last line, and the **process start time** that dates it
(the log's own stamps are time-only, with no date).

So: **a result carried beyond the conditions under which it was obtained** --
the rule this milestone recorded two rounds ago, appearing here **in the
instrument rather than in the prompt**. And the same error sits above it in the
kickoff: it stated correctly that the pair carried traffic during the Stage 2
round, then accepted a replaced-stand story that contradicted its own premise
instead of checking it.

**"No surviving host-side log records a successful attach" also loses its
force**, for the same reason: with per-start truncation, no log could record it.
The 2 September evidence is in the tree (`docs/measurements/m5-2-d1-select/`,
arm 3's `Ncat: Connected to 192.168.200.1:3013`), not on the host.

### What survives untouched

Everything host-side, because none of it depends on which instance is which: no
Hercules binary carries a capability (controls good); `hercifc` is setuid root
and actively maintained; **`EPERM` is the DESIGNED path** and therefore cannot
be the change; SIGCHLD refuted from `SigCgt`; the five refuted candidates; the
two controls that paid for themselves; `dpkg.log` reported as **not checked**;
and the `setcap` proposal, not run.

### The window, re-derived on the corrected premise -- and it is much tighter

Same box, same kernel, **same uptime (36 days, no reboot since 9 August)**, same
setuid `hercifc`, **same instance, same configuration, same files.**

| when | what | dated from |
|---|---|---|
| **2026-09-02** | CTCI **works** -- arm 3's confirmed connect | the record's own `**Date:**` line, not memory |
| **2026-09-03** | CTCI **fails**, build `11739-g60dd927e` | this document's first round |
| 2026-09-04 18:49 | `~/MVSCE` renamed `~/MVSCE-DEV` | directory mtime/ctime |
| 2026-09-06 11:03 | Hercules + `hercifc` rebuilt -> `11774-g59d8981c` | binary mtimes |
| 2026-09-06 12:15:36 | MVSCE-DEV starts on the new build -- CTCI **fails** | `ps -o lstart` for pid 805760 |

**Log coverage, since that is the control this round showed was missing:**
`~/MVSCE-DEV/hercules.log` covers **2026-09-06 12:15:36 onward, one start only**;
MVSCE-EXP from 2026-09-06 12:08:38; MVSCE-LAB from 2026-09-09 15:06:55. Nothing
on the host covers 2-3 September.

**The consequences, which the addendum had the facts for and did not draw
because the replaced-stand story filled the gap:**

1. **The change lies strictly between 2 and 3 September.** The rename (4 Sep)
   and the rebuild (6 Sep) are **both after the first failure** and therefore
   cannot be it. The failure additionally spans two builds.
2. **Configuration is no longer a candidate at all.** There is no newly drawn
   stanza to compare -- it is the *same file*, in the *same directory*, renamed.
   The configuration trail was the last surviving line of enquiry in the earlier
   round, and the corrected premise removes it.
3. What remains is **the host environment inside a one-day window**, and the
   signal.

### The signal hypothesis: decode verified, inference NOT supported

`SigCgt = 0x1000044cb` decodes to caught signals **1, 2, 4, 7, 8, 11, 15 and
33** (bits 0,1,3,6,7,10,14,32). Verified against this box's glibc rather than
assumed: `SIGRTMIN = 34`, so 32 and 33 are glibc-reserved (`SIGCANCEL`,
`SIGSETXID`). Bit 16 is clear, so **SIGCHLD stays refuted**.

**But the observation that motivated the SIGSETXID hypothesis is boilerplate,
so it is not evidence.** Control: bit 32 is **SET in every threaded program on
this box** -- `systemd-timesyncd`, `qemu-ga`, `containerd`, `dockerd`,
`gnome-keyring-daemon` -- because NPTL installs that handler in every
multithreaded glibc program. It says nothing about Hercules.

Two further checks, both negative:

- **`/proc/805760/timers` is empty** (rc 0; the file is present and
  world-readable, so this is a real "none"). **Its limit, stated:** it reflects
  the process **now**, eight days into the run, not the startup window in which
  the failure occurs -- a timer armed and disarmed during configuration would
  not appear.
- **Hercules does not call `setuid`/`setgid`/`setgroups`** anywhere in
  `impl.c` (one comment mentions setuid; there is no call), and the setuid
  `hercifc` is a **child after fork+exec**, which does not send `SIGSETXID` to
  its parent. So the mechanism the hypothesis needs is missing as well.

**The hypothesis is therefore recorded as unsupported, not as open.** That is
the second candidate signal this document has refuted -- SIGCHLD from its own
earlier text, SIGSETXID from the follow-up -- and in both cases the hedge is
what made the refutation cheap.

### Where it ends, again

Identifying the signal needs `strace` on the startup window. It is **not
installed**; installing it needs `sudo`, which needs a password we do not have.
**That is the one check that would close this**, and the round stops there
rather than fitting a story to the remainder.

**What a later reader should take from this document:** the CTCI pair worked on
**this exact instance**; the host-side arrangement is **ruled out by
measurement**; the configuration is **ruled out by identity**; and what is open
is a **signal inside a one-day window**, with two candidate signals refuted and
no third proposed.

---

## THIRD ROUND 2026-09-14 — the launch context, and SIGHUP refuted

**Appended; nothing above rewritten.** Read-only: no instance stopped or
started, no rebuild, no `sudo`, `~/hercules/hyperion` untouched.

Everything ruled out so far is **a file or a machine** -- the binary, its
capabilities, the device node, group membership, the kernel, the uptime, the
instance, its configuration. The **launch context** had never been examined,
and a file-by-file ladder walks past it because it is not a file.

### The decode, verified independently

Not taken from the prompt. Read from `/proc/805760/status` and named from the
**system's own signal table** (`python3 -c 'signal.Signals(n).name'`):

```
SigCgt = 00000001000044cb  ->  1 SIGHUP, 2 SIGINT, 4 SIGILL, 7 SIGBUS,
                               8 SIGFPE, 11 SIGSEGV, 15 SIGTERM, 33 (reserved)
```

SIGCHLD absent, so **its refutation stands**. And the set is now fully
accounted for in source: FPE/ILL/SEGV/BUS are `install_crash_handler()`
(`bootstrap.c:56-63`), INT/TERM are explicit (`impl.c:146`, `:184`), 33 is NPTL.
**SIGHUP is registered nowhere** -- the only occurrences in the entire tree are
a `strsignal.c` table entry and a `CHANGES` line reading **"28 Nov 2001 Remove
SIGHUP usage - Jan Jaeger"**. Hercules deliberately stopped using it a
quarter-century ago, and a handler is installed anyway.

### The launch context reads unchanged and unremarkable

| | MVSCE-DEV (*CTCI, fails*) | the other four |
|---|---|---|
| chain | `tmux(1292)` -> `bash(1293)` -> `start_mvs.sh` -> `hercules` | same shape, one via `bash -c source mvs_ipl` |
| controlling terminal | **present**, `(136,1)` = a pts | present, all of them |
| orphaned to init? | **no** -- parent `start_mvs.sh` alive | no |
| session | 1293 (the oldest pane) | 544438 … 1039440 (newer) |

*Controls:* `tty_nr` is shown to be **populated when a terminal exists** (three
processes listed with non-zero values), so a zero would have been a real "none"
rather than an unread field; and `ppid` 1 is resolved explicitly (`pid 1 comm =
systemd`), so "parent is init" could not read as "no parent recorded".

`start_mvs.sh` is three lines -- `ulimit -c unlimited` and the `hercules`
invocation -- with **no `nohup`, `setsid`, `trap`, `exec`, `disown`, no
background `&`, no `</dev/null`**. Its only difference from the other stands'
copies is the `ulimit` line, mtime **2026-08-18**, which predates the working
run and so cannot be the change.

**The differential the round wanted is UNAVAILABLE, and that is stated rather
than worked around:** only MVSCE-DEV configures CTCI at all, so no other
instance would reach the `tuntap` path and none can discriminate. The reading
stands alone.

### A real difference was found, and it still is not the change

The five instances do **not** share a caught-signal set:

```
MVSCE-EXP  (no CTCI)            00000001000044cb   SIGHUP caught
MVSCE-DEV  (CTCI, FAILS)        00000001000044cb   SIGHUP caught
MVSCE-LAB  (no CTCI)            00000001000044cb   SIGHUP caught
MVSTK5-REF (different binary)   00000001000044ca   SIGHUP NOT caught
MVSTK5-BLD (different binary)   00000001000044ca   SIGHUP NOT caught
```

Exactly one bit apart, and the bit is SIGHUP. The TK5 stands run Hercules
**4.9.1**; ours is **4.10.0**. So it is a property of the **build line**, not of
the stand, not of the launch, and not of the fork's own code (the git history
shows no fork commit adding it).

**Therefore SIGHUP is refuted as the change, by the same argument that killed
the other two:** the working run of 2 September was on
`4.10.0.11739-SDL-DEV-g60dd927e` -- the same 4.10.0 line -- so **SIGHUP was
caught on the day it worked as well as on the day it failed.**

It is left in the record as an **unexplained observation, not a lead**: a
handler is installed for a signal the source removed in 2001 and registers
nowhere by name, so it is installed with a numeric or computed signal number
somewhere not found by grep. That is worth knowing and is not evidence here.

### Three hypotheses, three refutations, and the boundary

SIGCHLD (bit clear), SIGSETXID (the motivating bit is NPTL boilerplate present
in every threaded program on the box; no `setxid` call in Hercules; timers
empty), and now SIGHUP (a 4.10.0 build-line property, present on the working
day). **No fourth candidate is proposed**, because there is no evidence for one
-- the remaining caught signals are faults a healthy process does not take and
shutdown signals that would not leave the instance running.

Identifying the signal needs `strace` on the startup window: **not installed**,
and installing it needs `sudo`. **That is where this stops, for the third
time**, and the record says so rather than fitting a story to the remainder.

### THE CHEAPEST REMAINING CHECK IS NOT ON THE BOX -- it is a question for Mike

Everything measurable on the host is identical across the working and failing
periods. **Did the way `MVSCE-DEV` is started change around 2-3 September?** A
different session or pane, a script where an interactive shell had been, a new
wrapper, a different terminal, something started under a different parent. A
reorganisation was plainly under way -- the rename followed on 4 September --
and a launch-method change on 3 September would fit **every measured fact**
without requiring anything on the box to have changed.

**This is asked, not guessed at.** The launch context as it stands today is the
one that fails; what it looked like on 2 September is not recoverable from the
box, because the shell history, the process and the logs from that period are
all gone.

---

## FOURTH ROUND 2026-09-14 — the latent-cause test, and two source findings

**Appended; nothing above rewritten.** Read-only: no instance stopped or
started, **no interface created, deleted or modified**, no rebuild, no `sudo`,
`~/hercules/hyperion` read only.

### §0 wording correction: SIGHUP is UNSUPPORTED, not refuted

The previous section said SIGHUP was "refuted". That is too strong and the
distinction matters. What the control established is that the caught set is a
**build-line property** (4.10.0 catches it, TK5's 4.9.1 does not) and that it
was caught on 2 September when the pair worked. **That refutes *"the signal
handling changed"*. It does not refute *"SIGHUP is the signal being
delivered"*** -- being caught is the **precondition**, not the event. With no
delivery mechanism anywhere in the launch context, the hypothesis is
**unsupported**. The earlier heading's conclusion stands; its wording does not.

### §1 -- THE READING: NO TUN DEVICE EXISTS, with Hercules running

Three independent readings agree:

- `ip -br link` shows **`lo`, `ens18`, `docker0`** and nothing else.
- `/sys/class/net/` contains the same three.
- **No interface carries `tun_flags`**, the sysfs marker present only on
  tun/tap devices.
- `ip tuntap list` is empty, rc 0.

*Controls:* sysfs is readable and the loop reached 3 devices; `lo/type` reads
`772`, so an empty tun scan is a real absence and not an unread tree.

**This is the second branch, not the interesting one.** No device was created,
so `hercifc` did not quietly succeed, there is no orphaned interface, and the
attach failure is real rather than a misread wait. **Mike's recollection --
that a device should be present while Hercules runs -- is settled the other
way by this reading**, which is why it was taken rather than assumed.

### §2 -- persistence: the configuration mechanism is refuted, one variant is not

**Nothing on the box sets up a persistent device.** No `ip tuntap`, `tunctl`,
`TUNSETPERSIST` or `IFF_PERSIST` in the three MVSCE trees, `~/.bashrc`,
`~/.profile`, `/etc/network` or any readable systemd unit. The only matches for
"persist" are English prose inside an MVS package file. *Coverage:*
`~/.bash_profile` does not exist and so was not searched; unreadable systemd
units are a limit. *Control:* the same grep finds `hercules` in
`start_mvs.sh`.

**The leaked descriptor cannot be the mechanism either.** Exactly one process
holds a `/dev/net/tun` fd -- MVSCE-DEV's own, fd 37, from its failed attach --
and **since §1 shows no device was ever created, that descriptor is not keeping
one alive**. *Coverage:* 27 `/proc/*/fd` directories scanned, 120 unreadable
(other users), a stated limit.

**So the CONFIGURATION form of the latent-cause hypothesis is refuted -- the
fourth refuted candidate in this document.** One variant survives and is
**not falsifiable from here**: a persistent device created by hand, ad hoc,
leaves no trace in any file, and its removal between 2 and 3 September would
fit every measured fact. The §3 test that would discriminate cannot be run
(below). **That is a second question for Mike, not a conclusion.**

### The standing "lost capability" refutation: UPHELD, and strengthened

It was upheld for a weaker reason than the right one. The review asked whether
it survives only if the setuid arrangement was the one in use -- a Hercules
carrying the capability would never have needed `hercifc`. **From the build
system, that configuration does not exist:**

```
if OPTION_CAPABILITIES
    setcap 'cap_sys_nice=eip' ./hercules      <- NOT cap_net_admin
    setcap 'cap_sys_nice=eip' ./herclin
    setcap 'cap_net_admin+ep' ./hercifc       <- the network capability goes HERE
endif
```

**`cap_net_admin` is never applied to `hercules` in any supported arrangement**
-- it goes to `hercifc`, exactly as the setuid bit does. So **the parent's
`TUNSETIFF` returns `EPERM` by design in BOTH arrangements**, and no lost
capability on `hercules` can be the change. Upheld, on better grounds.

**And the setuid bit was applied deliberately, which is now measured rather
than asserted.** `hercifc`: mtime `11:03:53.096`, ctime `11:04:00.828` -- the
mode/ownership was changed **7.7 seconds after the file was written**. *Control:*
`hercules` beside it has ctime **identical** to mtime. The build's own
`SETUID_HERCIFC` recipe produces `0750`+s (`-rwsr-x---`); the observed mode is
`4755`, which that recipe does not produce, and Hercules-Helper does nothing
about setuid (and is configured `opt_usesudo=false`). So a separate, deliberate
step set it.

### §3 -- the discriminating test: NOT APPLICABLE, and not manufactured

`tunsetiff-probe.c` against an **existing** device cannot be run, because §1
established there is no tun device to run it against. Pointing it at `docker0`
or `ens18` would return `EINVAL` (not a tun) and answer a different question.
**The precondition was not manufactured**: creating a device is a state change
and is forbidden here, and it would also destroy the very condition being
measured. Reported as not run, with the reason.

### §4.1 -- why the call does not restart, and a correction to the premise

**No handler in the tree sets `sa_flags` at all.** `grep` for
`sa_flags|SA_RESTART|SA_NODEFER|SA_SIGINFO` across every `.c` and `.h` returns
**nothing**; `sa_CRASH` is `= {0}`. So the four crash handlers
(`bootstrap.c:56-63`: FPE, ILL, SEGV, BUS) carry **no `SA_RESTART`**. The
handlers installed with `signal()` -- SIGINT, SIGTERM (`impl.c:146`, `:184`,
`:1636`, `:1645`), SIGPIPE ignored (`:1704`), and `printer.c:1745` -- get
`SA_RESTART` from glibc's BSD `signal()` semantics.

**But for this call that distinction does not matter, and the premise that it
does is wrong.** From `man 7 signal` on this box, primary source:

> *"Folgende Schnittstellen werden nach einer Unterbrechung durch einen
> Signal-Handler, **unabhängig von der Verwendung von SA_RESTART** nie erneut
> gestartet; sie schlagen immer mit dem Fehler EINTR fehl: … Schnittstellen,
> die Dateideskriptoren mehrfach nutzen: epoll_wait(2), epoll_pwait(2),
> poll(2), ppoll(2), **select(2)** und pselect(2)."*

**`select()` is never restarted, with or without `SA_RESTART`.** So "with
`SA_RESTART` the kernel restarts the call and none of this would have been
visible" is **false for `select()`** -- it would have been visible anyway.

**What that changes for the fix**, which is the reason it is worth stating:

- If the `EINTR` came from **`select()`**, `SA_RESTART` is irrelevant and an
  **explicit retry loop is the only correct fix**.
- If it came from **`read()`** -- which *is* restartable -- then a handler
  carrying `SA_RESTART` would have masked it, and none in this tree does.

Either way the retry is correct; the `SA_RESTART` framing is not a more precise
statement of the defect, it is a different and partly inapplicable one.
**Source finding only. Nothing changed.**

### §4.2 -- which call returned EINTR: the honest set is TWO, not one

The attribution to `select()` was an elimination argument over a set that was
never enumerated. Enumerated now, for every call between the fork and the
failure report:

| call | verdict |
|---|---|
| `socketpair()`, `fork()` | **excluded** -- neither returns `EINTR` |
| `write()` (in `VERIFY`) | **excluded by the observed messages.** A failed write leaves the child with no request, so `select` would time out at 5 s -> `rc == 0` -> **`HHC00135`** and errno forced to **`EPERM`**, reporting "Operation not permitted". We see neither that message nor that errno. |
| **`select()`** | **CANDIDATE** -- `rc = -1`, neither the `rc > 0` nor the `rc == 0` branch runs, `rc` stays -1, errno `EINTR` |
| **`read()`** | **CANDIDATE** -- reached when `select` returns > 0; `rc = -1` fails the `if (rc > 0)`, `rc` stays -1, errno `EINTR`. Identical report. |
| `close()`, `kill()`, `waitpid()` | **excluded by the code itself** -- `sv_err = errno` is saved *before* them and restored *after*, so any errno they set is overwritten. `waitpid` is `EINTR`-capable and still cannot be the source. |

**So the surviving set is `{select, read}`.** `select` is the more likely of the
two -- a `read` of data whose readability was just reported usually completes --
but **that is a plausibility argument, not evidence, and the attribution to
`select()` alone is NOT established.** Discriminating needs `strace`, which is
absent. An honest set of two.

### Two questions for Mike, both unanswerable from the box

1. **Did the way `MVSCE-DEV` is started change around 2-3 September?** (carried
   forward from the previous round)
2. **Was a tun device ever created by hand -- `ip tuntap add … persist` or
   equivalent -- and removed around then?** It would leave no file trace, and it
   would fit every measured fact.

---

## FIFTH ROUND 2026-09-14 — is the errno the failing call's? Source only

**Appended; nothing above rewritten.** No machine state touched, no privilege,
no interface created or removed, `~/hercules/hyperion` read only.

### §3 first -- the load-bearing premise, re-read: CONFIRMED, and made precise

Carried since the first round, and it holds. `tuntap.c:159-171`:

```
 158          rc = select (ifd[1]+1, &selset, NULL, NULL, &tv);
 159          if (rc > 0)
 160          {
 161              rc = read (ifd[1], &ctlreq, CTLREQ_SIZE);
 ...
 165          else if (rc == 0)
 ...
 171          }
```

`if (rc > 0) … else if (rc == 0) …` and **no `else`**. A `select` returning -1
falls through both arms, `rc` stays -1, and nothing specific to it is logged.

**But the precise statement matters, because it is not the defect the round
went looking for.** Falling through is what *preserves* `rc = -1` and the
errno; the missing arm costs a **retry**, not the attribution. It is a
**handling** defect, not a **reporting** defect.

### §2 -- THE RESIDUE HYPOTHESIS IS REFUTED. Outcome 1.

There **is** an errno save, and it is positioned exactly where it needs to be:

```
 173          /* clean-up */
 174          sv_err = errno;
 175          close (ifd[1]);
 176          kill (pid, SIGKILL);
 177          waitpid (pid, &status, 0);
 178          errno = sv_err;
```

`sv_err` is captured **immediately after** the select/read sequence and
restored **after** the cleanup calls. Walking every path that reaches line 184
with `rc < 0`:

| path | errno at line 174 |
|---|---|
| `select` returns -1 | the **select's** -- nothing between 158 and 174 can overwrite it |
| `select` > 0, `read` (161) returns -1 | the **read's** -- `memcpy` does not set errno |
| `select` returns 0 | `WRMSG` (168) may set errno, but **169 explicitly assigns `errno = EPERM`** afterwards -- and this path prints `HHC00135`, which is absent from every log |

**There is no window between a failing call and the capture in which anything
else can set errno.** So the printed value is the failing call's, and the
number 4 is not residue. **This is the kickoff's outcome 1, and it retires the
hypothesis that three refutations in a row were chasing a misread number.**

### AND YET THE SET GROWS FROM TWO TO THREE -- §4.2's elimination was still incomplete

§4.2 enumerated *"every call between the fork and the failure report"*. **The
failure can be reported without any fork happening at all**, and that is the
call nobody has examined:

```
  99      /* Try TUNTAP_ioctl first */
 100      rc = TUNTAP_IOCtl (fd, TUNSETIFF, (char *) hifr);
 104      if (0 > rc && errno == EINVAL)        <- guard 1
 108      if (0 > rc && errno == EPERM && ...)  <- guard 2, opens the hercifc block
 184      return rc;
```

At line 100 `TUNTAP_IOCtl` is a **plain `ioctl`** (`tuntap.h:218`; the redefine
to `IFC_IOCtl` is at `tuntap.c:508`, *after* this function ends at 499) -- no
wrapper, no retry, no errno handling. So:

> **If the ioctl at line 100 fails with an errno that is neither `EINVAL` nor
> `EPERM`, both guards are false, the `hercifc` block is skipped ENTIRELY, and
> line 184 returns -1 carrying that errno to the message at line 458.**

**Candidate 3 is therefore the `TUNSETIFF` ioctl itself returning `EINTR` -- in
which case no child was ever forked, no handshake happened, and no wait was
interrupted.** The whole `hercifc` story would be beside the point.

**What bears on it, honestly:** `tunsetiff-probe.c` measured `TUNSETIFF` ->
`EPERM`, 3 of 3, as the account Hercules runs under. That is evidence against
candidate 3 **in a quiet context**; it does not exclude it under startup
conditions, and whether this kernel can return `EINTR` from `TUNSETIFF` at all
is **not established** -- settling it needs kernel source or `strace`, neither
available here.

**The honest set is `{ioctl, select, read}`.** `select` remains the likeliest;
that is still not the same as established.

### Which earlier conclusions this affects -- and which it does NOT

**The three refuted signal candidates stand, and none was refuted for the wrong
reason.** Each refutation is independent of *which* call was interrupted:

- **SIGCHLD** -- `SigCgt` bit 17 clear; a child exiting cannot interrupt
  anything, on any call.
- **SIGSETXID** -- the motivating bit is NPTL boilerplate in every threaded
  program on the box, and Hercules calls no `setuid`/`setgid`/`setgroups`.
- **SIGHUP** -- a 4.10.0 build-line property, caught on 2 September when the
  pair worked (and **unsupported rather than refuted**, per the §0 correction).

**All three candidate calls fail with `EINTR`, and `EINTR` always implies a
signal was delivered.** So the signal question is real under every branch, and
Mike's two open questions keep exactly the weight they had. What candidate 3
would change is not *whether* a signal arrived but *what it interrupted* -- and
with it, whether `hercifc` was ever involved.

### The instrument-fault tally, put in the record rather than left in a report

Five faults in this investigation. **Four were caught by a control; one was
caught by review, and saying which is the point.**

| # | fault | caught by |
|---|---|---|
| 1 | `getcap` not on the non-interactive PATH -- an empty result would have read as "no capabilities" | **control** (a nonexistent-path probe, plus `tcpdump` as a known positive) |
| 2 | `/var/log/dpkg.log` had zero entries in the date range, so "no package activity" was uninterpretable | **control** (a count over the same range), reported as **not checked** |
| 3 | the quote-verification checker failed **closed** -- reported a present verbatim quote as missing, because it stripped whitespace but not `> ` markers | **control** (a phrase that should appear once, and did) |
| 4 | an `awk` quoting error printed an empty interface list during a read-only confirmation | **control** (the reading was re-run rather than accepted) |
| 5 | **"MVSCE-DEV started exactly once"** -- an artifact of `hercules -o` truncating the log at every start | **REVIEW, not a control.** The control that was run proved the search reached the files; it could not prove what period they covered. |

Fault 3 is the one worth carrying forward: **a verification that fails closed
would have had a correct document "fixed".** Fault 5 is the one worth being
honest about: **it is the only one a control did not catch**, and the reason is
that the control tested the wrong property.

### Coverage limits of this round

Source reading only, against `~/hercules/hyperion` at HEAD `59d8981c`, which is
the tree the running binary was built from (version string match established in
the addendum). Nothing here depends on machine state. Not established: whether
the kernel can return `EINTR` from `TUNSETIFF`; which of the three candidate
calls actually failed; and, still, which signal.
