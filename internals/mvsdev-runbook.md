# NSF — Live Target Runbook (`mvsdev`): the Full Record

The complete operational runbook for driving the live MVS target, with the
incidents that produced each rule. It used to be `CLAUDE.md` §5 "Live MVS target
& autonomous operation"; it moved here verbatim on 2026-10-14. The short form followed on
2026-10-19 and opens this file; the full record below has the signatures, the
examples and the reasoning.

**Written for mbt 2** (`make deploy`, `make test-mvs`, the `.env`). nsf370 moved
to mbt 3 on 2026-10-14; the tool-specific rules below are mbt 2 measurements
until re-measured.

**This file says how, not whether.** What may run on `dev1` without asking
(nsf370's own cycle only: `P NSFS`/`P NSFV`, `mbt deploy --target dev1`,
`S NSFS`/`S NSFV`, `mbt test --mvs --target dev1`) is set by the root
`CLAUDE.md`, "Working autonomously", and summarised in `CLAUDE.md` §5. Anything
else below that writes to the stand (a Hercules restart, a PROCLIB edit, ...)
asks first, whatever the full record's "without a human in the loop" says.

---

## Short form (moved from `CLAUDE.md` §5 on 2026-10-19)

This is the short form that `CLAUDE.md` §5 carried until 2026-10-19, updated for
mbt 3. The full record (written for mbt 2) follows it.

Short form; the signatures, examples and incidents are in
**`internals/mvsdev-runbook.md`**. Target: **MVSCE on Hercules on `mvsdev`**
(`mvsdev.lan`), mbt target `dev1`. Credentials live in the keychain and the
zowe `mvsdev` profile — never hardcode them.

**The runbook below was measured on mbt 2** (`make …`). The rules marked
*(mbt 2)* describe that tool's behaviour and are to be re-measured on mbt 3
before they are relied on; until then keep following them.

- **Build / deploy / test over mvsMF REST (no SSH):** `mbt deploy`
  (→ `NSF.LINKLIB`), `mbt test --mvs --only TSTX`. *(mbt 2)* Read the **full
  spool in `build/test-runner.spool`**, not just the matrix. mbt honoured
  **only `[build].cflags`** — a `-D`-gated device test goes there
  **temporarily** (revert before commit), and the source must be touched or a
  stale `.o` is reused.
- **Console = ZOWE + the console log.** `zowe zos-console issue command "<cmd>"`;
  direct output is unreliable, so read `ssh mvsdev "grep -A6 '<marker>'
  ~/MVSCE/mvslog.txt"` or `zowe zos-logs list logs --range 15m` (the syslog
  lags). STATS answers `NSF810/811I`, only DISPLAY answers `NSF800-802I`.
- *(mbt 2)* **Don't trust a "NO RC"/FAIL matrix on a long device job** — poll the jobid and
  read its spool. **`zowe zos-console` 500s on `P`/`S` but the command lands** —
  confirm from `mvslog.txt`, don't re-issue. **Back-to-back heavy CTCI runs
  degrade the pair** (MIH, `IGF991I/995I`) — a fresh pair, not a code fix.
- *(mbt 2)* **`--only` REPLACES the whole TESTLIB.** A rig not named that round draws
  `IEA703I 806-4` (absence looks like a broken rig), and two activities needing
  different test sets (e.g. the NSFV Stage-0 round and the row-2 procedure)
  need a redeploy between them.
- *(mbt 2)* **`make deploy` failed while an STC held `NSF.LINKLIB`, and the
  next run silently tested the OLD binary.** mbt 3 replaces members instead of
  deleting the library (its own help); that is a prediction until measured.
  Keep the order **`P NSFS`/`P NSFV` → `mbt deploy` → `S NSFS`**. Signature: a mid-chain `HTTP 500 … DELETE /restfiles/ds/NSF.LINKLIB`.
  Tell: *identical values across supposedly different builds* — re-run rather
  than reason about it.
- **A MIXED (half-)deploy tells the inverse: a value NEITHER pure build can
  produce** (#101: `EINVAL` from a dispatcher that has no `EINVAL` path). When a
  change alters a verb's error set, note the difference *before* deploying —
  and enumerate each pure build's error paths, or the tell is not loud.
- **`ssh mvsdev`** is keychain-independent (`~/.ssh/config` block, user `mike`);
  a reinstall changes the host key → `ssh-keygen -R mvsdev`. `ping`/`tcpdump`
  run without sudo; **sudo needs a password we do not have** — ask the user.
- **Hercules / CTCI:** config `~/MVSCE/conf/local/custom.cnf`; the running
  source is `~/hercules/hyperion/` (primary source beats docs). Pair `0500,0501
  CTCI 192.168.200.1 192.168.200.2` on `tun0` (guest `.1`, host `.2`). Device
  tests build with `-DNSFCTCI_CUU=0x0500 -DNSFCTCI_SRC=0xC0A8C801u
  -DNSFCTCI_DST=0xC0A8C802u`; the guest READ blocks until inbound traffic, so
  background `ssh mvsdev "ping -i 0.5 192.168.200.1"` and watch `tcpdump -ni
  tun0`. A missing `tun0` at Hercules start (`HHC01463E`) needs a full Hercules
  restart — never `devinit`.

---


### Live MVS target & autonomous operation (`mvsdev`)

Operational runbook for driving the live target without a human in the loop.
The target is **MVSCE on Hercules on the host `mvsdev`** (`mvsdev.lan`).
Credentials live in the un-committed `.env` and the zowe `mvsdev` profile —
never hardcode them.

- **Build / deploy / test go over mvsMF REST (no SSH):** `make deps` (allocate
  datasets), `make deploy` (→ `NSF.LINKLIB`), `make modules`, `make test-mvs
  ARGS="--only TSTX"`. `test-mvs` writes the **full job spool to
  `build/test-runner.spool`** — read it for the actual `printf`/message output,
  not just the PASS/FAIL matrix. MBT honours **only `[build].cflags`** (no
  per-test cflags — `mbtconfig.py`), so a `-D`-gated device test is compiled in
  by adding the define to `[build].cflags` **temporarily** (revert before commit).
- **Console commands = ZOWE + the console log.** Issue any command with
  `zowe zos-console issue command "<cmd>"` (default profile `mvsdev`): `S NSF`,
  `F NSF,DISPLAY`, `P NSF`, `D U`, … **Direct output is unreliable** (often only
  the first message line returns), so read the full result from the
  Hercules-captured console log: `ssh mvsdev "grep -A6 '<marker>'
  ~/MVSCE/mvslog.txt"` (`mvslog.txt` is the complete MVS console/syslog).
  For a time-bounded view of recent messages use `zowe zos-logs list logs
  --range 15m` (last 15 minutes; also how you wait out the syslog LAG — see M1
  live-run rules). Grep the RIGHT reply IDs: STATS answers `NSF810/811I`, only
  DISPLAY answers `NSF800-802I`. When gauging egress on a CTCI ping, `LNK1 out`
  is the device-transmit counter; since the issue-#21 fix (ADR-0025 pair
  sequencing) it tracks `NSFIP out`/`NSFICM outecho` exactly — a lasting lag
  now indicates a real regression ([[nsf370-ctci-write-tail-stall]]).
- **Live-run friction that each cost real time (issue #40 gates).** (1) `make
  test-mvs` can report **"NO RC" / FAIL while the job actually finishes CC 0** —
  the mbt job-poll times out on a long CTCI device job (e.g. TSTTCPD's guest waits
  out extra ~90 s accept windows). Don't trust the matrix: poll the jobid to
  `OUTPUT` (`zowe zos-jobs view job-status-by-jobid JOBnnnnn`) and read its spool
  (`zowe zos-jobs view all-spool-content JOBnnnnn`). (2) `zowe zos-console` **500s
  intermittently on `P NSF`/`S NSF`** ("Internal server error (abend recovery)")
  but the command **still lands** — confirm from `mvslog.txt` (`NSF830I`/`NSF000I`),
  don't re-issue blindly. (3) **Back-to-back heavy CTCI runs degrade the pair
  (MIH, IGF991I/995I)** — rapid-fire UDP echo throughput suffers on a degraded
  pair while ping and *paced* echo stay clean; the M3-5 rapid 1000/1000 echo needs
  a fresh/idle pair, not a code fix.
- **`zowe zos-files download data-set` skips a local file that already exists**
  (`... skipped as it already exists.`) unless `--overwrite` is given. A content
  check of a deploy or a restore then reads the PREVIOUS download, and with the
  output discarded nothing says so. Met in the #83 round: a correct deploy read as
  the old module. Download every verification copy into a new file or a fresh
  directory, and keep the command's output (`internals/measurements/m5-83/round.md`).
- **`make test-mvs ARGS="--only …"` REPLACES the whole TESTLIB** — it is not additive. Two
  consequences that cost time: a later `S <something>` for a module that round did not name
  draws `IEA703I 806-4 … MODULE ACCESSED <name>` (the rig is *absent*, and absence looks like
  a broken rig), and **two activities needing different test sets cannot run back to back**.
  The NSFV Stage-0 round and the row-2 client-death procedure
  (`internals/procedure-row2-client-death.md`) are exactly that pair: a **redeploy sits between
  them**, and a milestone gate wanting both must schedule that step.
- **`make deploy` fails while an STC holds `NSF.LINKLIB` — and the run afterwards
  silently tests the OLD binary.** Order is **`P NSFS` (or `P NSFV`) → `make deploy`
  → `S NSFS`**. **Signature:** a *mid-chain* `HTTP 500 Internal Server Error for
  DELETE /restfiles/ds/NSF.LINKLIB: {"rc":8,...,"message":"Dataset delete failed"}`
  — easy to miss when `deploy` is chained after `make modules`, because the lines
  that follow it still look like a normal deploy. **The tell, which is the part
  worth remembering:** *identical values across supposedly different builds.* Every
  "live" result after a failed deploy is about the previous module and nothing
  complains — this is the project's most expensive failure class in pure form.
  It cost a full diagnostic cycle in M5-2b2, where three runs of "different" routers
  returned byte-identical numbers. **Re-run rather than reason about it.**
- **The MIXED build tells the INVERSE, and it is the more dangerous of the two.**
  The tell above catches *"the deploy did not happen"*. A **half**-deploy — one
  side of a two-module pair replaced, the other not — catches nothing there,
  because a mixed build **looks like a working system**. Its tell is the
  opposite: **a value NEITHER pure build can produce.** #101 Stage 2's instance:
  a cross-AS `RQ_SELECT` returned `ERRNO=22` (`NSF_EINVAL`) immediately instead
  of parking, and the **pre**-#101 dispatcher has no `EINVAL` path at all — its
  only other `RQ_SELECT` error is `NSF_EOPNOTSUPP` (`src/nsfreq.c:920-926`; the
  `EINVAL` #101 added lives one module over, `src/nsfsel.c:214`). So
  NSFS was fixed while the client was reverted. **The habit that makes this a
  check rather than hindsight:** when a change alters a verb's **error set**,
  note the difference *before* deploying — it is a free build marker, and often
  the only surface available when a change adds no field and no message.
  **The prerequisite goes with it:** the tell only reads as *impossible* once
  each pure build's error paths have been enumerated. Written down without that
  enumeration it promises a loudness it will not deliver.
- **`ssh mvsdev`** (host-side work: `tun0` captures, reading Hercules source,
  `/proc`) is hardened **keychain-independent** — on-disk `~/.ssh/id_rsa`,
  `IdentityAgent none`, `accept-new`, via the `mvsdev` block in `~/.ssh/config`
  (user `mike`). A **reinstall changes the host key** → self-heal with
  `ssh-keygen -R mvsdev` then reconnect. `ping` and `tcpdump` (cap_net_raw) run
  **without sudo**; **sudo needs a password we do not have**, so anything needing
  root must be asked of the user.
- **Hercules / CTCI on this box.** Config `~/MVSCE/conf/local/custom.cnf`; the
  **exact running Hercules source** is at `~/hercules/hyperion/` (read
  `ctc_ctci.c` etc. directly — primary source beats docs). The CTCI pair is
  `0500,0501 CTCI 192.168.200.1 192.168.200.2` on `tun0` (guest `.1` / host
  `.2`), CUU 500/501 online, 502/503 offline. To drive a CTCI device test, build
  with `-DNSFCTCI_CUU=0x0500 -DNSFCTCI_SRC=0xC0A8C801u -DNSFCTCI_DST=0xC0A8C802u`;
  the guest READ blocks until inbound traffic, so background a continuous
  `ssh mvsdev "ping -i 0.5 192.168.200.1"` to trigger it and `tcpdump -ni tun0
  icmp` to see the WRITE (`/proc/net/snmp` `Icmp InEchos` is a no-sudo
  WRITE-egress proxy).
