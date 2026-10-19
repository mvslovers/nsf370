# nsf370 — Network Services Facility for MVS 3.8j

An event-driven networking subsystem for MVS 3.8j whose first responsibility
is a **native TCP/IP stack** — a from-scratch revival of the abandoned
`mvs38j-ip` project (not a port of its Xinu code). The goal is to run existing
applications (HTTPD, mvsMF) **unchanged, relink-only** over a native stack
instead of the Hercules X'75' hack, with full EZASOKET / PROFILE.TCPIP
compatibility.

> Status: **M0–M4 complete** — foundation, CTCI driver, IPv4/ICMP, sockets +
> UDP + EZASOKET, and TCP (state machine, data path, retransmission) all run
> live on Hercules. **M5 (Phase 2: the cross-AS `NSFS` subsystem)** is in
> progress: the app↔stack transport is proven both ways on the live target — an
> SSI probe (Stage-0a) and, for **unauthorized relink-only apps**, a private-SVC
> probe (Stage-0a′, ADR-0038). See `CLAUDE.md` §7 for the milestone status and `docs/milestone-history.md` for the full record.

## Documentation

| Document | Role |
|---|---|
| `docs/Project-Brief-v2.md` | Why / scope / constraints / milestones (frozen) |
| `docs/Architecture-Specification.md` | How: interfaces, data structures, lifetimes |
| `CLAUDE.md` (repo root) | Operating rules, conventions, milestone status |
| `docs/adr/` | Architecture Decision Records |

## Toolchain

- **cc370** — host cross-compile suite (S/370 objects on Linux/macOS)
- **libc370** — the cc370 target C library (sysroot; not a project dependency)
- **mbt 3** (MVS Build Tools) — build orchestration; the `mbt` program on
  your `PATH`, configured by `mbt.toml`

## Quick start

```sh
git clone https://github.com/mvslovers/nsf370.git
cd nsf370

mbt test                      # build + run the portable tests natively (no MVS)
mbt build --all --tests       # cross-compile every load module and test module
mbt test --mvs                # deploy + run the tests on MVS
mbt deploy                    # put the load modules into NSF.DEV.LINKLIB
mbt doctor                    # check the toolchain and the MVS connection
```

The MVS system comes from `~/.mbt/targets.toml` (`mbt target list`); an mbt 2
style `.env` (template: `.env.example`) converts with
`mbt target import .env --name NAME`. Run `mbt --help` for the full command
list.

## Layout

```
mbt.toml       mbt 3 project (modules, tests, toolchain, deploy target)
src/           portable C sources (nsf*.c)
asm/           HLASM sources (nsf*.asm)
include/       public headers (one per component)
test/          dual host+MVS tests; test/mvs/ MVS-only; test/asm/ asm callers
cfg/           sample PROFILE.TCPIP members
jcl/           install / SAMPLIB jobs
docs/          brief, spec, ADRs   (CLAUDE.md is at the repo root)
```

## License

TBD.
