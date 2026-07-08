# Architecture Decision Records

ADR-0001 … ADR-0013 are summarized normatively in
`../Architecture-Specification.md` §18; standalone files will be split
out as they are revisited. ADR-0014 onward are authored directly here.

| ADR | Decision | File |
|-----|----------|------|
| 0001 | Event-driven executive | spec §18 |
| 0002 | CTCI first | spec §18 |
| 0003 | Phase 1 in-process | spec §18 |
| 0004 | Fixed memory pools | spec §18 |
| 0005 | No Xinu code | spec §18 |
| 0006 | ESTAE + teardown checklists from M0 | spec §18 |
| 0007 | Prefix NSF, subsystem NSFS | spec §18 |
| 0008 | Single-owner buffers, no refcounting | spec §18 |
| 0009 | Two buffer classes (256/2048) | spec §18 |
| 0010 | Delta-queue timers, no wheel | spec §18 |
| 0011 | 100 ms tick via a single re-armed STIMER (not STIMERM) | ADR-0011-100ms-tick-via-stimer.md |
| 0012 | No IP fragmentation/reassembly in v1 | spec §18 |
| 0013 | Toolchain: cc370 + libc370, MBT V2 | spec §18 |
| 0014 | Build model & repo layout follow MBT V2 conventions | ADR-0014-build-model-and-repo-layout.md |
| 0015 | NSFMM pool regions via libc370 malloc (not raw GETMAIN) | ADR-0015-region-acquisition-via-libc370-malloc.md |
| 0016 | Shared nsftime seam (nsf_now + nsf_taskid) | ADR-0016-shared-platform-time-and-task-seam.md |
| 0017 | Timer wakeup via the async STIMER exit (not a subtask) | ADR-0017-timer-wakeup-async-stimer-exit.md |
