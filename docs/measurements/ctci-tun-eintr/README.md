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
