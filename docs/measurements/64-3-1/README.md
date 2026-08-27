# 64-3-1 measurement artifacts

**Read `docs/nsf-64-3-1-dontswap.md` first.** The three `gate-*.log` files are NOT
interchangeable, and one of them is not evidence.

| file | standing |
|---|---|
| `gate-restored.log` | **EVIDENCE.** The shipping build, pinned. 163 samples, 9 min, one sampler, timestamps monotonic. |
| `gate-reverted.log` | **EVIDENCE.** `DONTSWAP` removed, NSFS swappable. Same shape, same duration — the like-for-like control. **This arm is why the gate is reported as NOT discriminating** (§3.2). |
| `gate-pinned.log` | **SUPERSEDED — DO NOT CITE.** Contaminated: two sampler processes wrote this one file (a `nohup` run believed dead, plus a later run that truncated it underneath), so it holds 228 interleaved samples and a non-monotonic timestamp at index 223. Every *value* in it is a genuine reading of NSFS and all of them agree with `gate-restored.log`, but the window, the sample count and the `ASCBFMCT` change-points are untrustworthy. See §3.2a. |

| file | |
|---|---|
| `stageA-readings.txt`, `stageA-console.log` | Stage A: the probe, two runs, byte-identical |
| `nsfswatch.py` | the sampler. Credentials come from `MBT_MVS_USER` / `MBT_MVS_PASS` in the environment — **never argv**, which would put the password in `ps` for the life of the run (§3.2a) |
| `runarm.sh` | one arm. Kills stray samplers and **asserts none survive** before starting exactly one — the direct fix for the contamination above |
| `TSTRQXCA.jcl`, `TSTRQXCB.jcl` | the two-client contention rounds (`TSTRQXC PARM='A'` / `PARM='B'`) |
