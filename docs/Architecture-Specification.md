# NSF — Network Services Facility for MVS 3.8j
## Architecture Specification

*Version 1.13 — Draft for implementation. Companion document to the frozen
Project Brief v2 (`docs/Project-Brief-v2.md`). The filename is intentionally
unversioned; the current version is stated here and in the changelog
(Appendix A).*

---

## Document Relationship

| Document | Answers | Status |
|---|---|---|
| **Project Brief v2** | Why the project exists, scope, constraints, milestones | **Frozen** |
| **Architecture Specification** (this document) | Component responsibilities, interfaces, data structures, control flow, object lifetime | Living document, versioned |
| **ADRs** (`docs/adr/`) | Why individual architectural decisions were made | Append-only |

This specification is the technical blueprint for implementation. Where it
resolves questions left open in the Project Brief (naming, buffer classes,
timer design), the resolution is recorded here and justified in an ADR.

---

## 00 — Architecture Goals

These goals are binding. Every design and code review decision is evaluated
against them, in this order of precedence:

1. **Deterministic memory usage.** All storage is acquired at initialization
   from fixed, configurable pools. The maximum memory footprint of the stack
   is computable from the configuration *before* it starts. No allocation
   ever occurs on a packet or timer hot path.
2. **No hidden allocations.** Protocol code never calls GETMAIN, `malloc`,
   or any storage service directly. The Memory Manager owns all storage;
   every object type has a named pool with a documented per-object cost.
3. **Predictable execution.** One event loop, run-to-completion handlers,
   no preemption inside the stack. An event handler never blocks and never
   loops unboundedly.
4. **Layer isolation.** Components communicate only through the interfaces
   defined in this document. Protocols do not touch drivers; drivers do not
   touch sockets; nobody touches raw storage.
5. **Testability.** All protocol logic compiles and runs on Linux/macOS with
   a native compiler. MVS-specific code is confined to the `mvs/` layer and
   hidden behind the same interfaces the host shims implement.
6. **IBM compatibility.** The API is EZASOKET, the configuration is
   PROFILE.TCPIP syntax, the operator interface follows MVS conventions.
   A z/OS systems programmer should feel at home; an application written
   against EZASMI/X'75' should relink and run.
7. **Incremental implementation.** Every milestone ends in something that
   runs and can be demonstrated. Foundation services are built to *minimum
   viable* state, then hardened under real protocol load.
8. **Fail safe, fail observable.** Every task has ESTAE coverage; every
   abnormal path releases its resources (buffers, I/O, ENQs) and leaves a
   diagnosable trace. A crash must never require a Hercules restart to
   clean up.

Goal 8 is added relative to the Project Brief (see ADR-0006): experience
with long-running STCs on MVS 3.8j (e.g. secondary ABENDs during task
termination caused by resource leaks) shows that recovery design cannot be
retrofitted.

---

## 01 — Overview

### 1.1 System Structure

NSF ("Network Services Facility") is an event-driven network executive.
Its core is a dispatcher plus seven foundation services; TCP/IP is its
first — not its only possible — protocol suite.

```
        Applications (HTTPD, FTP, mvsMF, user programs)
                 │
   ┌─────────────┴─────────────┐
   │  EZASOKET Library (NSFEZA) │   ← IBM-compatible API, Ch. 15
   └─────────────┬─────────────┘
                 │  NSFRQE request blocks (Ch. 10.4)
   ┌─────────────┴─────────────┐
   │  Request Manager (NSFREQ)  │   ← request transport + validation
   ├───────────────────────────┤
   │  Socket Manager  (NSFSOC)  │   ← protocol-independent sockets, Ch. 10
   ├─────────────┬─────────────┤
   │  TCP (NSFTCP)│ UDP (NSFUDP)│   ← transports, Ch. 12–13
   ├─────────────┴─────────────┤
   │  IPv4 + ICMP (NSFIP/NSFICM)│   ← network layer, Ch. 11
   ├───────────────────────────┤
   │  Device Abstraction(NSFDEV)│   ← Ch. 9
   ├──────────┬───────┬────────┤
   │ CTCI     │ LCS   │ HOST    │   ← drivers (HOST = pcap/loopback for tests)
   └──────────┴───────┴────────┘
   ─── Foundation services (Ch. 2–8), used by all layers ───
   Memory (NSFMM) · Buffers (NSFBUF) · Queues (NSFQUE) ·
   Timers (NSFTMR) · Dispatcher (NSFEVT) · Trace (NSFTRC) · Stats (NSFSTS)
```

### 1.2 Naming Conventions (resolves Open Question "prefix")

- **Component prefix:** `NSF` (Network Services Facility). Three characters,
  does not collide with IBM `EZA*` module or message space, and reflects the
  post-review identity of the project (see ADR-0007).
- **Load modules / CSECTs:** `NSF` + up to 5 characters (`NSFMM`, `NSFBUF`,
  `NSFTCP`, …). Source files lowercase (`nsfmm.c`, `nsftcp.c`, `nsfctci.asm`).
- **Message IDs:** `NSFnnns` — `NSF` + 3-digit number + severity
  (`I`/`W`/`E`/`S`), e.g. `NSF001I NSF INITIALIZATION COMPLETE`.
  Number ranges are reserved per component (000–099 executive, 100–199
  memory/buffers, 200–299 devices, 300–399 IP/ICMP, 400–499 UDP,
  500–599 TCP, 600–699 sockets/API, 700–799 config, 800–899 operator,
  900–999 recovery).
- **Subsystem name (Phase 2):** `NSFS` (4 characters, decided before M5;
  `STCP` remains the documented historical alternative).
- **External API names:** unchanged IBM names (`EZASOKET` entry point,
  PROFILE.TCPIP statement keywords) — compatibility outranks branding at
  the API boundary.

### 1.3 Execution Model

Two MVS tasks exist inside the stack's address space in **both** phases:

| Task | Contents | Created by |
|---|---|---|
| **Executive task** | Event loop, all protocol logic, timers, operator commands | Mainline (jobstep task in Phase 1 STC; or ATTACHed subtask when linked into an application) |
| **I/O exits** | Device I/O completion (IOS exit / attention), timer exit (STIMER exit) | MVS, asynchronously |

Everything above the driver bottom half runs on the executive task,
single-threaded, run-to-completion. The asynchronous exits do exactly two
things: move a pre-allocated element onto an interrupt-safe queue (CS-based)
and POST an ECB. All real work happens later on the executive task.

**Phase 1 (in-process):** the stack is ATTACHed as a subtask inside the
application's address space. The application task and the executive task
communicate through the same NSFRQE request blocks that Phase 2 will
transport via SSI — only the transport changes, never the request format
(see ADR-0003 and Ch. 10.4).

**Phase 2 (subsystem):** the executive task lives in its own STC address
space registered as subsystem `NSFS`; NSFEZA becomes a stub that ships
NSFRQEs across address spaces (SSI/SVC + cross-memory POST).

### 1.4 Canonical Control Flows

Understanding these four flows is understanding the system.

**(a) Inbound packet (e.g. ICMP echo request):**
```
CTCI attention/completion (async exit)
  → exit: enqueue completed I/O element (CS), POST device ECB, (re-drive READ)
executive task: WAIT returns
  → NSFEVT dequeues element → PBUF handed to NSFDEV bottom half
  → nsfdev_input(): strips CTCI block/segment header → EVT(PACKET_RECEIVED)
  → nsfip_input(): validate, checksum, demux by protocol
  → nsficmp_input(): echo request → build reply *in the same PBUF*
  → nsfip_output(): fill header, route lookup → nsfdev_send()
  → CTCI driver: build CCW chain from PBUF, EXCP
  → WRITE completion exit later frees the PBUF back to NSFBUF
```
Buffer ownership moves driver→IP→ICMP→IP→driver; zero copies.

**(b) Application send (TCP, Phase 1):**
```
App task: EZASOKET('SEND',...) in NSFEZA
  → build NSFRQE {fn=SEND, sockid, data ptr/len, ECB}
  → enqueue to request queue (CS), POST request ECB, WAIT on NSFRQE ECB
executive task: EVT(SOCKET_REQUEST)
  → NSFREQ validates → NSFSOC → tcp_usr_send():
      copy user data into PBUFs (the one unavoidable copy: crossing the
      app/stack boundary), append to TCB send queue, try to transmit
  → complete NSFRQE (retcode/errno), POST application ECB
App task resumes with EZASOKET return values.
```

**(c) Timer expiry (TCP retransmission):**
```
STIMER exit (async): POST timer ECB           /* nothing else */
executive task: EVT(TIMER_EXPIRED)
  → nsftmr_run(ticks): pop all due TMR elements from delta queue
  → each TMR fires its callback, e.g. tcp_rexmit_to(tcb)
  → nsftmr_run() re-arms STIMER for the next deadline (or none)
```

**(d) Operator command:**
```
MODIFY NSF,DISPLAY,SOCKETS
  → comm task posts CIB ECB (QEDIT chain)
executive task: EVT(OPERATOR_COMMAND)
  → NSFOPR parses CIB → dispatch DISPLAY/TRACE/STATS/STOP handler
  → WTO multi-line response with NSF8xx message IDs
```

### 1.5 Memory Budget (default configuration)

Everything below the 16 MB line; the target total for the stack, all pools
included, is **≤ 1.5 MB** in the default configuration, leaving room for
applications in Phase 1. The authoritative numbers live in Ch. 2.5 and are
kept current as structures evolve; the discipline is: *every control block
declares its size in a comment, and a compile-time size assertion enforces
it.*

```c
/* size discipline — used for every control block */
#define NSF_SIZE_ASSERT(type, size) \
    typedef char nsf_assert_##type[(sizeof(type) == (size)) ? 1 : -1]
```

### 1.6 Build Toolchain & Environment

The project targets one toolchain, and the repository, build flow and test
harness are all shaped by it (see ADR-0013). This is a hard dependency, not
an interchangeable detail — it determines directory layout, dependency
resolution and how tests are driven, so it is stated here up front.

- **C compiler: `cc370`** — the complete host-side cross-compilation suite
  (compiles S/370 objects on Linux/macOS). This replaces the older
  `c2asm370` C-to-assembler path; NSF does **not** use `c2asm370`.
- **C runtime: `libc370`** — the `cc370` **sysroot** (target C library:
  headers, `libc.a`, `crt0/crt1/crtm` startfiles, SYS1/crent macros),
  provided by the toolchain. It replaces `CRENT370`. Because it is the
  sysroot, `libc370` is **not** a `[dependencies]` entry in `project.toml`
  (ADR-0014, correcting v1.1, which had listed it as a resolved dependency).
- **Assembler:** IFOX00 on the target for HLASM modules (unchanged).
- **Linkage editor:** IEWL on the target (unchanged).
- **Build orchestration: MBT V2 (MVS Build Tools, v2)** — the Python-based
  build system already used across the mvslovers ecosystem (e.g. mvsMF).
  A two-line `Makefile` (`MBT_ROOT := mbt` + `include $(MBT_ROOT)/mk/mbt.mk`)
  wires it in; `mbt` is a git submodule. MBT owns:
  - **Repository shape:** a top-level `project.toml` declares modules,
    sources, tests, dependencies and build products; local MVS connection
    settings live in an un-committed `.env` (copied from `.env.example`).
  - **Build flow (real MBT V2 target names):** `deps` (resolve
    dependencies, allocate target datasets, TSO RECEIVE), `test-host`
    (native build + run of the portable tests — see next bullet),
    `modules`/`lib` (cross-compile with cc370 + assemble HLASM on MVS),
    `package` (TRANSMIT/XMIT the load library), `deploy` (upload + RECV370
    on MVS), `test-mvs` (deploy + run the suite on MVS). There is **no**
    `bootstrap`/`build`/`link` target — those names in v1.1 predate the real
    MBT V2 target set (ADR-0014; `deps` is the former `bootstrap`).
  - **Both build worlds run through MBT.** The host build is a first-class
    MBT target (`make test-host`, native compiler) configured by a `[host]`
    table in `project.toml`; a `[host].replace` map swaps MVS-only CSECTs
    (`asm/*.asm`) for host shims (`src/*_host.c`) as they appear. This
    corrects v1.1, which placed the host build outside MBT in a separate
    `host.mk` (ADR-0014).
  - **Dependencies** (cc370 macro libraries, later mvslovers modules) are
    resolved by MBT rather than vendored; `libc370` is the sysroot, not a
    dependency.

Consequences that this specification depends on elsewhere:
- The **host unit tests** (Ch. 16, Level 0/1) build with a native compiler
  and need **no MVS**, so they are the CI gate — but they are still driven
  through MBT via `make test-host`, not a separate `host.mk` (ADR-0014).
  MBT drives **both** worlds; what distinguishes Level 2+ is that it reaches
  a live MVS 3.8j over IP, not that it "uses MBT" while Level 0/1 does not.
- The repository layout in Ch. 16.2 is a **flat** MBT `project.toml` layout
  (`src/*.c`, `asm/*.asm`), not the grouped tree described in v1.1
  (ADR-0014).

---

## 02 — Memory Manager (NSFMM)

### 2.1 Responsibilities

- Owns **all** dynamic storage of the stack. Acquires one contiguous region
  per pool via GETMAIN at initialization; never again afterwards.
- Provides fixed-size object pools with O(1) allocate/free.
- Accounts for every object: in-use count, high-water mark, allocation
  failures, cumulative allocations (leak detection by differential).
- Detects corruption (eyecatchers) and double-free in debug builds.
- Reports usage via NSFSTS and the DISPLAY MEMORY operator command.

### 2.2 Public Interface

```c
/* nsfmm.h — all sizes in bytes, all counts are objects */
int      mm_init(const MMCFG *cfg);              /* init-time only        */
MMPOOL  *mm_pool_create(const char *name8,       /* init-time only        */
                        USHORT objsize, USHORT count);
void     mm_init_complete(void);                 /* seal: no more pools   */
void    *mm_alloc(MMPOOL *pool);                 /* NULL = pool exhausted */
void     mm_free(MMPOOL *pool, void *obj);
void     mm_stats(const MMPOOL *pool, MMSTATS *out);
void     mm_shutdown(void);                      /* FREEMAIN everything   */
```

Rules:
- `mm_pool_create` is legal only between `mm_init` and `mm_init_complete`;
  it ABENDs (user completion code) if called later. `mm_init_complete` is the
  seal that marks the end of the initialization window (the executive's
  startup sequencing, ch. 5, calls it once all component inits have created
  their pools). This *enforces* the no-runtime-allocation goal instead of
  merely documenting it.
- `mm_alloc` returning NULL is a normal, expected condition. Every caller
  must handle it (drop packet + count, reject connection, fail API call
  with ENOBUFS). Exhaustion is graceful degradation, never an ABEND.

### 2.3 Internal Data Structures

```c
typedef struct mmobj {                /* prefix on every pooled object  */
    UCHAR   eye[2];                   /* 2  0xC5D6 alloc / 0xC6D9 free  */
    UCHAR   poolid;                   /* 1  index into pool table       */
    UCHAR   flags;                    /* 1                              */
    struct mmobj *next;               /* 4  free-list link (when free)  */
} MMOBJ;                              /* 8 bytes overhead per object    */
NSF_SIZE_ASSERT(MMOBJ, 8);

typedef struct mmpool {
    char    name[8];                  /* 8  eyecatcher, e.g. "TCPTCB " */
    MMOBJ  *freelist;                 /* 4                              */
    UCHAR  *region;                   /* 4  GETMAINed base              */
    USHORT  objsize;                  /* 2  payload size (w/o MMOBJ)    */
    USHORT  count;                    /* 2  total objects               */
    USHORT  inuse;                    /* 2                              */
    USHORT  hiwater;                  /* 2                              */
    UINT    allocs;                   /* 4  cumulative                  */
    UINT    frees;                    /* 4  cumulative                  */
    UINT    fails;                    /* 4  exhaustion events           */
    UCHAR   poolid;                   /* 1                              */
    UCHAR   rsvd[3];                  /* 3                              */
} MMPOOL;                             /* 40 bytes per pool              */
NSF_SIZE_ASSERT(MMPOOL, 40);
```

- Pool region layout: `count` × (`sizeof(MMOBJ)` + `objsize`), objects
  8-byte aligned.
- Free list is LIFO (hot objects stay cache/TLB-friendly; and on S/370,
  recently used pages stay resident).
- **Region acquisition (both builds):** a single primitive
  (`mm_region_get`/`mm_region_free`) draws each region from libc370 `malloc`
  and releases it with `free`. On MVS 3.8j that `malloc` resolves to `GETMAIN`
  below the 16 MB line (24-bit, so inherently AMODE/RMODE 24); the host build
  uses the identical calls. NSFMM therefore stays pure portable C — no raw
  GETMAIN SVC in assembler — and this is the only place in the whole stack a
  storage service is touched. See **ADR-0015**.

### 2.4 Object Lifetime

An object belongs to exactly one of: the free list, or one owner component.
There is no reference counting anywhere in NSF (see ADR-0008): shared
ownership in 16 MB with C is how leaks are born. Components hand objects
off explicitly; the trace facility can log every alloc/free/handoff of a
pool when `TRACE MEMORY` is on.

### 2.5 Default Pool Budget

| Pool | Object | Size (payload) | Count | Total (incl. 8B hdr) |
|---|---|---:|---:|---:|
| `BUFSMALL` | small PBUF + 256B data | 288 | 64 | ~18 KB |
| `BUFLARGE` | large PBUF + 2048B data | 2080 | 128 | ~261 KB |
| `SOCKET` | socket CB | 128 | 64 | ~8.5 KB |
| `TCPTCB` | TCP connection CB | 256 | 32 | ~8.3 KB |
| `UDPPCB` | UDP CB | 64 | 32 | ~2.3 KB |
| `RQE` | API request element | 96 | 64 | ~6.5 KB |
| `EVT` | event element | 24 | 128 | ~4 KB |
| `IOELEM` | driver I/O element | 64 | 32 | ~2.3 KB |
| **Sum** | | | | **≈ 311 KB** |

All counts are PROFILE.TCPIP-configurable (Ch. 14). Code, stack, trace ring
and MVS overhead push the realistic default footprint toward ~1 MB — well
inside budget. The per-connection marginal cost (1 TCB + typically 2–8
large buffers) is documented so operators can size `TCPTCB`/`BUFLARGE`
deliberately (Project Brief §7.2 "max connections").

---

## 03 — Buffer Manager (NSFBUF)

### 3.1 Responsibilities

- Provides packet buffers (PBUF) in two size classes, allocated from NSFMM
  pools `BUFSMALL` (256 B data) and `BUFLARGE` (2048 B data).
  Two classes, not one, because ACK-sized TCP segments and ICMP messages
  would waste 87 % of a 2 KB buffer, and 24-bit storage is the scarcest
  resource (resolves Open Question "buffer sizes"; ADR-0009).
- Implements the single-owner ownership model and headroom management.
- Supports chaining for the rare case where a payload exceeds one buffer
  (TCP send queue); protocol code treats chains through helpers, never by
  walking pointers itself.

### 3.2 Public Interface

```c
/* nsfbuf.h */
int     buf_init(void);                    /* create pools; 0 ok, !=0 fail */
PBUF   *buf_alloc(USHORT hint_len);        /* picks class by hint       */
void    buf_free(PBUF *b);                 /* frees whole chain         */
int     buf_prepend(PBUF *b, USHORT n);    /* claim n bytes of headroom */
int     buf_trim_head(PBUF *b, USHORT n);  /* consume from front        */
int     buf_trim_tail(PBUF *b, USHORT n);  /* drop from the chain's tail */
USHORT  buf_copyin (PBUF *b, const void *src, USHORT n);  /* app→stack  */
USHORT  buf_copyout(const PBUF *b, void *dst, USHORT n);  /* stack→app  */
PBUF   *buf_chain_append(PBUF *head, PBUF *tail);
USHORT  buf_chain_len(const PBUF *head);
void    buf_reset_rx(PBUF *b);             /* inbound seam (M1)         */
```

Notes (added at M0-3, the same way `mm_init_complete` was recorded in §2.2):

- `buf_init` is init-window only — it calls `mm_pool_create`, legal solely
  between `mm_init` and `mm_init_complete`. It creates `BUFSMALL`/`BUFLARGE`
  at the default counts (small 64, large 128; NSFCFG override is M0-7) and
  remembers the two `MMPOOL*` for class→pool selection on free. It **returns
  0 on success, non-zero if either pool could not be created**, so the
  executive startup (M0-8) can refuse to start rather than run with NULL pools;
  it does no operator reporting itself (that belongs to the startup path).
- `buf_reset_rx` is the inbound seam consumed by the driver at M1: it moves
  `data` to `start` (no headroom, since nothing is prepended on the way up)
  and reopens the whole `B`-byte data area (`size == B`, `len == 0`).
- **Layout constants (fixed; ADR-0009 for the class sizes):**
  `NSFBUF_SMALL_DATA = 256`, `NSFBUF_LARGE_DATA = 2048`,
  `NSFBUF_HEADROOM = 64`. Class selection is by hint plus default headroom:
  `BUFSMALL` when `HEADROOM + hint_len ≤ 256` (i.e. `hint_len ≤ 192`), else
  `BUFLARGE`. Outbound allocation opens `data = start + HEADROOM`, `len = 0`,
  `size = B − HEADROOM`.
- The pooled-object payload is `sizeof(PBUF) + B` (288 / 2080 on the target,
  matching §2.5). It is computed from `sizeof(PBUF)` at run time rather than
  hardcoded, because `sizeof(PBUF)` is 32 only on the S/370 target — a host
  test build has wider pointers — and using the same `sizeof` for both the
  pool object size and `start` keeps them self-consistent on either platform.
- `buf_prepend` grows `size` by `n` (it moves `data` back); `buf_trim_head`
  shrinks it by `n` (it moves `data` forward); `buf_trim_tail` leaves `size`
  unchanged (`data` is fixed). All three keep the head's `chainlen` in step.
  `buf_chain_len` is the authoritative walk-sum; the `chainlen` field is a
  head-only cache maintained incrementally and re-checked against the walk-sum
  under `NSF_DEBUG`.
- `buf_trim_tail` acts on the **logical tail of the packet**: on a chain it
  trims the last element (`chain == NULL`), not the buffer it is handed —
  trimming the head element would silently drop bytes from the middle of the
  packet, and the `chainlen` self-check cannot catch that (head `len` and
  `chainlen` stay mutually consistent). Single-buffer behaviour is unchanged.
  `buf_prepend`/`buf_trim_head`, by contrast, act on the head, where headers
  are added and consumed.

### 3.3 Buffer Layout and Structure

```
 ┌──────────┬───────────────┬──────────────────────────┬──────┐
 │ MMOBJ 8B │ PBUF hdr 32B  │ headroom (64B default)   │ data │
 └──────────┴───────────────┴──────────────────────────┴──────┘
                             ↑ start                    ↑ data
```

Inbound: the driver receives into `start + headroom` is *not* needed —
drivers receive at `start`, because nothing is prepended on the way up.
Outbound: transport layers allocate with default headroom so IP (20 B) and
any future link header (LCS: 14 B Ethernet) can be *prepended* without
copying.

```c
typedef struct pbuf PBUF;
struct pbuf {
    QELEM   q;            /* 8  queue linkage (socket rx q, dev q, ...) */
    PBUF   *chain;        /* 4  next PBUF of same packet, or NULL       */
    UCHAR  *data;         /* 4  first valid byte                        */
    USHORT  len;          /* 2  valid bytes at *data (this buffer)      */
    USHORT  size;         /* 2  forward capacity from data == (start+B)-data */
    USHORT  chainlen;     /* 2  total valid bytes (head element only)   */
    UCHAR   class;        /* 1  BUFSMALL / BUFLARGE                     */
    UCHAR   flags;        /* 1                                          */
    UINT    allocseq;     /* 4  sequence number for leak diagnosis      */
    void   *dbg_owner;    /* 4  last owner (trace builds; 0 in prod)    */
};                        /* 32 bytes */
NSF_SIZE_ASSERT(PBUF, 32);
```

### 3.4 Ownership Lifetime

```
alloc → (owner: allocator) → handoff… → terminal owner frees
inbound:  driver → IP → TCP/UDP/ICMP → socket rx queue → RECV copies out → free
outbound: transport allocs → IP → driver → WRITE-complete exit queues it
          → executive frees after completion event
```

Two hard rules, enforced in review:
1. A function either *keeps* a PBUF or *passes it on* — never both, never
   neither. Every interface in this document states which.
2. Only the executive task frees buffers. Exits queue them for freeing.

---

## 04 — Queue Library (NSFQUE)

### 4.1 Responsibilities

One correct queue implementation used by every component: intrusive,
doubly-linked, O(1) enqueue/dequeue/remove. Plus a second, deliberately
separate primitive: an interrupt-safe single-producer stack for the
exit→mainline handoff.

### 4.2 Public Interface & Structures

```c
typedef struct qelem { struct qelem *next, *prev; } QELEM;  /* 8 bytes */
typedef struct queue { QELEM head; USHORT count, maxcount; } QUEUE;

void    q_init(QUEUE *q, USHORT maxcount);   /* 0 = unbounded (avoid!)  */
int     q_enq(QUEUE *q, QELEM *e);           /* rc!=0: queue full       */
QELEM  *q_deq(QUEUE *q);                     /* NULL: empty             */
void    q_remove(QUEUE *q, QELEM *e);        /* unlink from middle      */
#define Q_EMPTY(q)   ((q)->count == 0)
#define Q_ENTRY(e, type, member)  /* container-of macro */ \
        ((type *)((char *)(e) - offsetof(type, member)))
```

Bounded queues (`maxcount`) are the norm: socket receive queues, listen
backlogs and device queues all reject rather than grow — memory
determinism again.

**Interrupt-safe handoff (exit side):**

```c
/* nsfxq.h — CS-based LIFO push from exits, drained by executive */
void    xq_push(XQ *xq, QELEM *e);   /* CS loop; callable disabled/exit  */
QELEM  *xq_drain(XQ *xq);            /* atomic swap; executive reverses  */
```

The exit-side push is ~5 instructions (LOAD, ST, CS, retry) and is the
**only** synchronization primitive exits ever use, apart from POST. On the
host build, `xq_push` is implemented with a compiler atomic; the interface
is identical. All mainline-only structures need no locking at all
(single executive task, run-to-completion — Project Brief §3.8).

### 4.3 Lifetime

QELEMs are always embedded in pooled objects (PBUF, TMR, EVT, NSFRQE);
the queue library never allocates.

---

## 05 — Event Dispatcher (NSFEVT)

### 5.1 Responsibilities

- Owns the main loop: WAIT on the ECB list, translate POSTs into events,
  dispatch to registered handlers, run due timers, then transmit queued
  output.
- Owns orderly startup and shutdown sequencing of all components.
- Anchors the recovery environment (ESTAE) for the executive task (Ch. 17).

### 5.2 Interface

```c
typedef enum {
    EV_PACKET_RECEIVED, EV_PACKET_SENT,   /* device bottom half        */
    EV_TIMER_EXPIRED,                     /* timer manager             */
    EV_SOCKET_REQUEST,                    /* request manager           */
    EV_OPERATOR_CMD,                      /* MODIFY/STOP via CIB       */
    EV_SHUTDOWN,
    EV_MAX
} EVTYPE;

typedef void (*EVHANDLER)(EVT *ev);
int   evt_register(EVTYPE t, EVHANDLER h);        /* init-time only     */
int   evt_post(EVTYPE t, void *p1, UINT u1);      /* executive-task use */
void  evt_mainloop(void);                         /* never returns until
                                                     EV_SHUTDOWN        */
```

```c
typedef struct evt {
    QELEM   q;          /* 8                                   */
    USHORT  type;       /* 2                                   */
    USHORT  flags;      /* 2                                   */
    void   *p1;         /* 4  e.g. PBUF*, NSFRQE*              */
    UINT    u1;         /* 4  e.g. device index                */
    UINT    rsvd;       /* 4                                   */
} EVT;                  /* 24 bytes */
NSF_SIZE_ASSERT(EVT, 24);
```

### 5.3 Main Loop Contract

```
for (;;) {
    WAIT on ECBLIST { devECB[n], timerECB, requestECB, cibECB, stopECB }
    drain exit handoff stacks (xq_drain) → internal event queue
    while ((ev = q_deq(&evq)) != NULL)
        handlers[ev->type](ev);          /* run to completion */
    nsftmr_run(elapsed_ticks);           /* fire due timers    */
    nsfdev_kick_output();                /* start pending I/O  */
}
```

Handler rules (binding):
- Never WAIT, never loop over unbounded input, never call `mm_pool_create`.
- May generate further events (`evt_post`) — processed in the same drain,
  with a drain budget (default 64 events) after which the loop re-WAITs
  with the ECBs re-checked, so a flood cannot starve timers.

Operator wiring (M0-8). The `cibECB` slot is filled by `evt_set_operator(ecb,
drain)`: the console ECB (EXTRACT COMM) joins the ECBLIST, and the loop calls the
operator `drain` **once per pass, unconditionally — not gated on the ECB bit**.
MVS can queue the startup CIB (CIBSTART) *without* POSTing the ECB; gating the
drain on the bit would hold the single CIB slot and reject every later MODIFY
with `IEE342I TASK BUSY`. The drain walks the CIB chain, dispatches each
(`CIBSTOP`→`EV_SHUTDOWN`; `CIBMODFY` text→`nsfopr_dispatch`), and QEDITs it. The
CIB/QEDIT and WTO seams reuse libc370 (`__gtcom`/`__cibget`/`__cibdel`, `wto`) —
ADR-0018.

### 5.4 Shutdown Sequence

`P NSF` / `MODIFY NSF,STOP` → EV_SHUTDOWN →
1. stop accepting NSFRQEs (fail with ESHUTDOWN),
2. abort sockets (TCP: send RST — no lingering timers),
3. quiesce devices (halt I/O, wait for outstanding EXCPs),
4. cancel timers, 5. dump final statistics, 6. `mm_shutdown()`, 7. return.

Every step has a timeout; a hung device cannot hang the shutdown (Goal 8).

M0-8 realizes steps 4/5/6/7 (`nsftmr` disarm → free pending events → `mm_shutdown`
→ return; steps 1–3 are stubs until NSFRQE/sockets/devices exist). None of the
realized steps performs an unbounded wait, so there is nothing to time out yet;
the **per-step timeout framework lands with M1 device quiesce** (step 3, the only
step that waits on outstanding EXCPs) — that is where "a hung device cannot hang
shutdown" becomes load-bearing.

---

## 06 — Timer Manager (NSFTMR)

### 6.1 Responsibilities

- One timer service for the whole stack: TCP retransmit/persist/keepalive/
  2MSL, delayed ACK, ARP cache (later), DNS timeouts (later).
- Driven by a single MVS `STIMER` (real interval, SVC 47), re-armed to the
  head delta. The timer exit does nothing but POST the timer ECB; all timer
  processing happens on the executive task. (Correction, M0-5: 3.8j provides
  `STIMER`, not `STIMERM` — see §6.3 and ADR-0011.)

### 6.2 Design Decision: sorted delta queue, not a timer wheel

A wheel buys O(1) insertion at the price of a slot array — memory spent
even when idle. NSF's active-timer population is small (a handful per TCP
connection, dozens overall), so a sorted delta list is O(n) insert with
tiny n, zero fixed cost, and trivially correct. (ADR-0010; revisit only if
profiling ever shows insertion cost — unlikely at these scales.)

### 6.3 Interface & Structure

```c
typedef void (*TMRFN)(void *arg);

typedef struct tmr {
    QELEM   q;          /* 8   delta-queue linkage             */
    UINT    delta;      /* 4   ticks after predecessor         */
    TMRFN   fn;         /* 4                                   */
    void   *arg;        /* 4   e.g. TCB*                       */
    UCHAR   state;      /* 1   IDLE / PENDING                  */
    UCHAR   rsvd[3];    /* 3                                   */
} TMR;                  /* 24 bytes — EMBEDDED in owner CBs    */
NSF_SIZE_ASSERT(TMR, 24);

void  nsftmr_init(void);                                   /* reset / disarm    */
void  tmr_start (TMR *t, UINT ticks, TMRFN fn, void *arg); /* restart ok        */
void  tmr_cancel(TMR *t);                                  /* idempotent        */
void  nsftmr_run(UINT ticks);      /* advance by elapsed ticks (main loop)      */
UINT       nsftmr_count(void);     /* inspection seam (operator DISPLAY, tests) */
const TMR *nsftmr_peek (UINT i);   /* i==0 = head/soonest; NULL past the tail   */
```

- **Tick = 100 ms** (resolves Open Question "granularity"). `STIMER`
  resolution on MVS 3.8j is 0.01 s units, so 100 ms is comfortably
  representable (BINTVL = 10); TCP RTO minimum will be 1 s (RFC-conservative)
  and delayed ACK 200 ms — both fit. An M0 test job validates actual `STIMER`
  behavior under Hercules before this is frozen (ADR-0011).
- TMR structures are **embedded** in their owner control blocks (a TCB
  carries its four timers inline) — timer arming can therefore never fail
  and never allocates.
- `nsftmr_run` re-arms `STIMER` for the head delta only when the queue is
  non-empty; an idle stack takes zero timer interrupts.

**M0-5 implementation notes** (folded in; the interface above is refined):
- **`STIMER`, not `STIMERM`.** MVS 3.8j / TK4- provides the single-interval
  `STIMER` (SVC 47) + `TTIMER CANCEL`; `STIMERM` is an MVS/SP addition absent
  from the 3.8j macro library. A single `STIMER REAL` re-armed to the head
  delta reproduces the intended behaviour, and NSFTMR only ever needs one
  active interval. The published `STIMERM` in this chapter, ADR-0016 and the
  Brief is corrected here and in ADR-0011.
- **`nsftmr_run(UINT ticks)`** takes the elapsed tick count. On MVS the
  executive passes the number of ticks the expired `STIMER` represented (the
  head delta it was armed for); the host has no `STIMER`, so tests inject
  elapsed ticks directly. (Refines the published `nsftmr_run(void)`; §5.3's
  main loop passes the elapsed count.)
- **Added helpers.** `nsftmr_init` resets the queue and disarms the platform
  timer (startup / test reset). `nsftmr_count` / `nsftmr_peek` are the
  always-on inspection seam (the M0-8 operator DISPLAY and the host tests read
  queue order and each timer's delta through them), mirroring NSFTRC.
- **Platform seam `nsfstim.h`.** Arming lives behind `nsftmr_plat_arm` /
  `_disarm` / `_ecb`: `asm/nsfstim.asm` on MVS (STIMER + the POST exit),
  `src/nsfstim_host.c` on the host (no-op that records arms for the tests),
  swapped by `[host].replace` — the NSFXQ / NSFTIME pattern. Per this chapter,
  only `nsftmr_run` re-arms at M0-5; tmr_start/tmr_cancel that shorten the head
  re-arm once the executive loop owns the pending-timer state (M0-6).
- **`nsftmr_run` semantics.** Detaches and marks each due timer IDLE before its
  callback runs (a callback may safely re-arm the timer that fired); a cancel
  hands its delta to the successor while a fire consumes it; `tmr_start` clamps
  `ticks` to a minimum of 1.

### 6.4 Lifetime

A TMR lives exactly as long as its owner CB. `tmr_cancel` is mandatory in
every owner-teardown path and is safe to call in any state — this is the
class of rule whose violation produces use-after-free from timer pops, so
teardown checklists in Ch. 12/13 enumerate every embedded timer.

---

## 07 — Trace Facility (NSFTRC)

### 7.1 Responsibilities

- Per-subsystem trace flags: `IP TCP UDP ICMP DRIVER MEMORY TIMER SOCKET
  API CONFIG` — settable at startup (PROFILE) and runtime (MODIFY).
- Disabled cost: one TM instruction (flag test in a macro).
- Output: in-storage ring buffer always; optional live WTO or SYSPRINT
  spool per flag. Ring survives into dumps (eyecatcher `NSFTRACE`), which
  makes post-mortem analysis of an ABEND possible without reproducing it.
- Hex dump helper for packet-level tracing (driver, IP).

### 7.2 Interface

```c
#define TRC(comp, fmt, ...) \
    do { if (nsftrc_flags & TRCF_##comp) \
         nsftrc_write(TRCF_##comp, (fmt), ##__VA_ARGS__); } while (0)

void  nsftrc_init(void);                 /* zero the ring + stamp eyecatcher  */
void  nsftrc_enable(UINT flags);         /* set flags  (PROFILE/MODIFY later) */
void  nsftrc_disable(UINT flags);        /* clear flags                       */
void  nsftrc_write(UINT flag, const char *fmt, ...);
void  nsftrc_hexdump(UINT flag, const char *tag,
                     const void *p, USHORT len);
UINT          nsftrc_count(void);        /* live entries (dump / DISPLAY)     */
const TRCENT *nsftrc_peek(UINT i);       /* i==0 oldest; NULL past the end    */
const void   *nsftrc_ring_base(void);    /* dump anchor (NSFTRACE eyecatcher) */
```

Ring buffer: configurable size (default 64 KB), fixed-size 128-byte
entries (timestamp, flag, task, truncated text) — deterministic, no
allocation, oldest-overwritten.

> **M0-4 implementation note.** Delivered in `include/nsftrc.h` /
> `src/nsftrc.c`. The `TRC` macro uses `, ##__VA_ARGS__` (a gnu99 extension) so
> a flag-only call — `TRC(TCP, "listening")` — compiles with no trailing
> arguments. `TRCENT` is fixed at exactly 128 bytes and **pointer-free**:
> `NSFTIME ts` (8) · `UINT flag` (4) · `UINT task` (4) · `char text[112]`, so
> `NSF_SIZE_ASSERT(TRCENT, 128)` holds on host and target alike (a host unit
> test also asserts `sizeof` at run time — the only host-side size guarantee,
> since the macro's value check is target-only). The ring is one static BSS
> object (header + 512 entries = 64 KB), zeroed and stamped `NSFTRACE` by
> `nsftrc_init`; all ring arithmetic uses the compile-time entry count, so a
> write before init cannot divide by zero. `nsftrc_write` does **not** test the
> flag (the `TRC` macro is the gate); `nsftrc_hexdump` **self-gates** on its
> flag, as there is no macro wrapper. **Single writer** at M0-4: no asynchronous
> exits exist yet, so the ring needs no locking; M1 must add CS-reserved slot
> indexing (as NSFXQ reserves its stack top) before an exit may trace. The
> timestamp (`nsf_now`) and numeric task id (`nsf_taskid`) come from the shared
> platform seam `include/nsftime.h` (**ADR-0016**), reused by NSFTMR at M0-5.
> No new control-block-size or milestone-contract change beyond adding `TRCENT`.

---

## 08 — Statistics Manager (NSFSTS)

### 8.1 Responsibilities

- A registry of named 32-bit counters, registered per component at init.
- Rendered by `DISPLAY STATS` / future NETSTAT; reset via operator command.
- Counters are plain increments on the executive task — no atomicity needed
  except the few maintained by exits, which live in exit-owned cache lines
  and are sampled, not shared.

### 8.2 Interface

```c
typedef struct stsctr { char name[12]; UINT value; } STSCTR;   /* 16 B */

void    sts_init(void);                                 /* zero + eyecatcher */
STSCTR *sts_register(const char *component, const char *name);
void    sts_reset(void);                                /* zero all values   */
UINT    sts_count(void);
UINT    sts_render(char *buf, UINT bufsize);            /* DISPLAY STATS      */
#define STS_INC(c)      ((c)->value++)
#define STS_ADD(c, n)   ((c)->value += (UINT)(n))
```

Minimum counter set per component is specified in the owning chapter
(e.g. IP: in/out/errors/badcksum/badlen/noroute/fragdropped).

> **M0-4 implementation note.** Delivered in `include/nsfsts.h` /
> `src/nsfsts.c`. `STSCTR` stays exactly 16 bytes and pointer-free; the owning
> `component` lives in an internal registry record that *wraps* the counter, so
> grouping in the render costs the 16-byte counter nothing. The registry is one
> static BSS array of `NSFSTS_MAX` (128) records behind an `NSFSTATS`
> eyecatcher. `sts_register` is init-window only and returns `NULL` when the
> table is full — a build-time miscount the caller reports, never an abend,
> mirroring `mm_pool_create` past its pool max. A returned `STSCTR*` is **stable
> for the run** (the array never moves and registration only appends), so a
> component caches it once and `STS_INC`s it on the hot path with no lookup.
> `sts_render` writes `component name value` lines (registration order) into a
> caller-supplied buffer, truncating only at whole-line boundaries and returning
> the byte count; the M0-8 operator `DISPLAY STATS` hookup calls it. No new
> control-block-size or milestone-contract change.

---

## 09 — Device Abstraction & Drivers (NSFDEV / NSFCTCI / NSFLCS / NSFHOST)

### 9.1 Responsibilities

- `NSFDEV` defines the driver contract and owns the device table.
- Drivers move raw frames between MVS channel I/O and PBUFs. They know
  nothing above IP; IP knows nothing below `netdev`.

### 9.2 Driver Contract

```c
typedef struct netdev NETDEV;
typedef struct devops {
    int (*init)    (NETDEV *dev, const DEVCFG *cfg);
    int (*start)   (NETDEV *dev);              /* first READ driven      */
    int (*send)    (NETDEV *dev, PBUF *b);     /* takes ownership of b   */
    int (*shutdown)(NETDEV *dev);              /* halt I/O, drain        */
} DEVOPS;

struct netdev {
    char     name[8];        /* LINK name from PROFILE          */
    DEVOPS  *ops;
    USHORT   cuu;            /* device address                  */
    UCHAR    type;           /* CTCI / LCS / HOST               */
    UCHAR    state;          /* DOWN/STARTING/UP/QUIESCING      */
    UINT     ipaddr;         /* HOME address (network order)    */
    USHORT   mtu;
    USHORT   flags;
    QUEUE    sendq;          /* bounded outbound queue          */
    XQ       doneq;          /* exit→mainline completed I/O     */
    UINT     ecb;            /* device ECB (in main ECB list)   */
    STSCTR  *ctr_in, *ctr_out, *ctr_ierr, *ctr_oerr;
    void    *priv;           /* driver private block            */
};
```

Ownership: `send` takes the PBUF unconditionally — on immediate error the
driver frees it and counts. Inbound PBUFs are allocated by the driver
(bottom half, executive task) and handed up via `EV_PACKET_RECEIVED`.

**Device table & loop seam (M1-2).** `NSFDEV` owns a fixed device table (no
runtime allocation): `dev_register(cfg, ops)` claims a slot, initializes the
common fields (state `DOWN`, bounded `sendq`, empty `doneq`, per-device
counters) and calls `ops->init`; `dev_find` (by LINK name) / `dev_find_cuu` /
`dev_by_index` / `dev_foreach` iterate it; `dev_start` / `dev_send` /
`dev_shutdown` dispatch through `ops` (with the send-ownership rule above).
The executive loop is **driver-agnostic** — it never names a concrete driver.
`NSFDEV` registers three hooks with the loop (`evt_set_devices`, mirroring
`evt_set_operator`): `nsfdev_collect_ecbs` appends the device ECBs to the
ECBLIST at loop entry; `nsfdev_poll_input` drains every `doneq` up to
`EV_PACKET_RECEIVED` (`p1` = PBUF*, `u1` = device index) once per pass before
dispatch, clearing the device ECB first (lost-wakeup safe) and, on EVT-pool
exhaustion, dropping+counting rather than abending; `nsfdev_kick_output`
(§5.3 step 5) drains each UP device's `sendq` through `ops->send`. `dev_send`
issued from outside a loop pass wakes the loop (`nsfevt_wake`) so pending
output is kicked. `dev_shutdown` calls `ops->shutdown` (stop the producer)
then drains `sendq` + `doneq`, freeing every held PBUF (the leak gate). The
`DEVCFG` passed to `ops->init` carries the common interface fields
(name/cuu/type/ipaddr/mtu) plus a `drvcfg` pointer for driver-private config.

### 9.3 CTCI Driver (NSFCTCI) — first concrete driver

Split into the standard exit/mainline halves:

- **Top half (HLASM):** EXCP with CCW chains for READ and WRITE against
  the emulated 3088 pair; I/O completion exit pushes the IOELEM onto
  `doneq` (xq_push) and POSTs the device ECB. Runs the absolute minimum
  in exit state.
- **Bottom half (C, executive task):** parses/creates the CTCI frame
  structure, converts to/from PBUFs, re-drives the next READ, starts
  queued WRITEs.

**Frame format (normative — verified against Hercules `ctc_ctci.c` /
`ctcadpt.h`; in current Hercules the CTCI code is split out of `ctcadpt.c`
into `ctc_ctci.c`).** The device is an emulated **3088** CTC (SID `0x3088`,
model `0x08`) presented as a **read/write subchannel pair** on consecutive
CUUs (e.g. `0E20` read, `0E21` write). NSF drives it with CCW opcodes
`0x02` READ (inbound, host→guest), `0x01` WRITE (outbound, guest→host),
`0x07` CONTROL, `0x03` NOP, `0x04` SENSE.

The device buffer is a **chain of blocks**; each block starts with a block
header and carries one or more segments, one per IP datagram:

```
CTCIHDR  (block header)
  +0  hwOffset   H'2'   byte offset of the NEXT block in the buffer;
                        0x0000 marks the last block of the chain
CTCISEG  (segment header, one per IP frame)
  +0  hwLength   H'2'   segment length INCLUDING this 6-byte header
  +2  hwType     H'2'   frame type, always 0x0800 (IPv4)
  +4  _reserved  H'2'   always 0x0000
  +6  <data>            the raw IP packet
```

A block is `[CTCIHDR] ([CTCISEG][IP]) …`; blocks chain through `hwOffset`,
the final block having `hwOffset = 0x0000`. All halfwords are **big-endian**
— native S/370 order — so NSF builds and reads them with no byte swapping.

Two Hercules behaviours the driver must honour: (1) it is the guest READ CCW
(`CTCI_Read`) that appends the terminating `hwOffset = 0x0000` block, so the
bottom half walks the chain to that zero offset; (2) MIH complaints after
long idle — the driver keeps a READ outstanding and treats HIO/restart as a
normal path.

Buffer sizing: default `0x5000` (20 KB), min `0x4000`, max `0xFFFF`;
`MAX_CTCI_FRAME_SIZE = buffer − sizeof(CTCIHDR) − sizeof(CTCISEG) − 2`. The
HLASM top half moves this buffer verbatim over the READ/WRITE CCWs and is
format-blind; the C bottom half builds/parses `CTCIHDR`/`CTCISEG` around the
PBUF, so the framing is host-testable via the NSFHOST loopback driver.

### 9.4 HOST Driver (NSFHOST)

Same `DEVOPS` contract, implemented on Linux/macOS. This is what makes the
full stack runnable in CI: IP/ICMP/UDP/TCP (M2–M4) are developed and tested
against NSFHOST on the host before Hercules is ever involved. It moves **raw
IP packets** — there is no CTCI `CTCIHDR`/`CTCISEG` framing here (that is
CTCI-only; §9.3); a host TUN device already presents raw IP.

**Modes** (`HOSTCFG.mode`, selected at `dev_register`):
- `LOOPBACK` (default) — a pure in-memory loopback for unit tests: a
  transmitted PBUF is handed back inbound, so a full send→receive cycle runs
  with no OS networking. This is the CI path.
- `TUN` — a real host TUN interface (Linux `/dev/net/tun`, macOS utun) for
  live traffic. Compiled only with `-DNSFHOST_TUN`; `dev_register` fails for
  this mode otherwise.
- `PCAP` — reserved for a pcap capture source (not yet implemented).

**Async producer (host analog of the CTCI I/O-completion exit).** Inbound
frames are delivered by a reader **thread**, not synchronously: on a received
frame it hands a PBUF to the device `doneq` (`xq_push` — lock-free) and POSTs
the device ECB — exactly the push+post the MVS CTCI exit will do (M1-3). The
executive loop then drains the doneq up to `EV_PACKET_RECEIVED`
(`nsfdev_poll_input`). So the whole `doneq → EV_PACKET_RECEIVED` integration
is validated across a real thread boundary before any device exists, and M1-3
swaps **only** the producer. In loopback mode the reader relays the same PBUFs
the send side fed to an internal wire (copy-free — the wire carries raw IP);
in TUN mode it reads the tun fd. NSFMM is touched only on the executive task
(never on the reader), so the host test does not race the pools — the same
single-task storage rule the MVS design relies on.

No MVS code compiles into the host build; no host code compiles into the MVS
build (Ch. 16.2). `src/nsfhost.c` (the pthread driver) is swapped in for the
host build; the MVS build compiles a placeholder (`src/nsfhost_plat.c`) whose
`nsfhost_ops()` returns NULL — there is no host driver on MVS (use CTCI/LCS),
and a portable test that references it still cross-links.

### 9.5 LCS Driver (NSFLCS) — Milestone 6

Ethernet framing + ARP dependency (Ch. 11.6). Deferred; exists in this
document only as proof that the abstraction holds: implementing `DEVOPS`
and an ARP module must require **zero** changes above `nsfip_output()`'s
next-hop resolution hook.

---

## 10 — Socket Layer (NSFSOC) and Request Manager (NSFREQ)

### 10.1 Responsibilities

- NSFSOC: protocol-independent socket objects; the demultiplex point
  between "application world" (requests, blocking semantics) and
  "protocol world" (events, callbacks).
- NSFREQ: receives NSFRQE request blocks from the API library, validates
  them, drives them through NSFSOC, completes them (retcode/errno + POST).

### 10.2 Socket Object

```c
typedef struct sockcb SOCKCB;
typedef struct protops {                  /* per-protocol callbacks */
    int (*attach) (SOCKCB *s);            /* SOCKET                 */
    int (*bind)   (SOCKCB *s);
    int (*connect)(SOCKCB *s, NSFRQE *r);
    int (*listen) (SOCKCB *s, int backlog);
    int (*send)   (SOCKCB *s, NSFRQE *r);
    int (*recv)   (SOCKCB *s, NSFRQE *r);
    int (*close)  (SOCKCB *s, NSFRQE *r);
    int (*detach) (SOCKCB *s);            /* final resource release */
} PROTOPS;

struct sockcb {
    USHORT   id;            /* index into socket table          */
    USHORT   gen;           /* generation counter (stale fd use) */
    UCHAR    domain, type, proto, state;
    UINT     laddr, faddr;  /* network order                    */
    USHORT   lport, fport;
    PROTOPS *ops;
    void    *pcb;           /* TCB* or UDPPCB*                  */
    QUEUE    rxq;           /* PBUFs ready for RECV (bounded)   */
    QUEUE    acceptq;       /* TCP: established, un-ACCEPTed    */
    NSFRQE  *pend_recv;     /* parked blocking RECV (or NULL)   */
    NSFRQE  *pend_accept;
    NSFRQE  *pend_connect;
    UINT     owner_ascb;    /* Phase 2: client identity/cleanup */
    STSCTR  *ctr;           /* per-socket not kept; global ctrs */
};  /* target ≤ 128 bytes incl. rsvd — SIZE_ASSERT in code */
```

Socket table: fixed array (`SOCKET` pool, default 64). Descriptors handed
to applications are `(gen << 16) | id`, so a stale descriptor after
close/reuse fails with EBADF instead of hitting the wrong connection.

### 10.3 Blocking Semantics (the parked-request pattern)

The stack never blocks. A blocking RECV on an empty queue *parks* the
NSFRQE on the socket (`pend_recv`) and returns to the event loop; the
application task stays in WAIT on the NSFRQE's ECB. When data arrives,
the protocol callback completes the parked request. SELECT is the same
pattern across multiple sockets with a shared ECB. Non-blocking mode
(FCNTL/FIONBIO) simply completes immediately with EWOULDBLOCK.

Lifetime rule: a parked NSFRQE is owned by the socket. Socket teardown
(CLOSE, RST, shutdown) must complete every parked request with an error —
this is on the teardown checklist, same class as timer cancellation.

### 10.4 NSFRQE — the phase-independent request block

```c
typedef struct nsfrqe {
    QELEM   q;              /* 8                                  */
    UCHAR   eye[4];         /* "RQE "                             */
    USHORT  fn;             /* RQ_SOCKET, RQ_BIND, ... RQ_SELECT  */
    USHORT  flags;
    UINT    sockdesc;       /* (gen<<16)|id                       */
    void   *ubuf;           /* user buffer (Phase 1: same space;  */
    UINT    ulen;           /*  Phase 2: keyed cross-memory move) */
    UINT    p1, p2, p3;     /* fn-specific (addr, port, backlog…) */
    INT     retcode;        /* EZASOKET RETCODE                   */
    INT     errno_;         /* EZASOKET ERRNO                     */
    UINT    ecb;            /* completion ECB (app-side)          */
    UINT    reqid;          /* trace correlation                  */
} NSFRQE;                   /* 64 B core; pool objsize 96 for growth */
```

This structure is the **phase boundary contract**: Phase 1 transports it
via an in-address-space queue + POST; Phase 2 via SSI/SVC with
cross-memory POST and MVCK/MVCSK for `ubuf` moves. Protocols and sockets
never know which transport delivered it. Changing NSFRQE after M3
therefore requires an ADR.

### 10.5 Lifetime

SOCKCB: `mm_alloc` at RQ_SOCKET → live → teardown checklist (cancel
timers via pcb detach, flush rxq/acceptq PBUFs, complete parked NSFRQEs,
free pcb, bump `gen`, `mm_free`). The checklist is a single function
(`soc_destroy`) — never inlined ad hoc — so no teardown path can forget a
step. (Direct lesson from ENQ/resource-leak ABENDs in long-running MVS
servers.)

---

## 11 — IPv4 and ICMP (NSFIP / NSFICM)

### 11.1 Responsibilities

- Input: header validation, checksum, option skip (options are validated
  for length and otherwise ignored), demux by protocol to TCP/UDP/ICMP.
- Output: header construction, identification counter, checksum, next-hop
  resolution via routing table, hand-off to the device.
- ICMP: echo responder, error generation (dest unreachable, time
  exceeded), and inbound error delivery to transports (M5).

### 11.2 Interfaces

```c
/* upward: called by device bottom half via EV_PACKET_RECEIVED */
void  nsfip_input(NETDEV *dev, PBUF *b);        /* takes ownership */

/* downward: called by transports; fills IP header into headroom  */
int   nsfip_output(PBUF *b, UINT src, UINT dst,
                   UCHAR proto, UCHAR ttl);      /* takes ownership */

/* ICMP */
void  nsficmp_input(NETDEV *dev, PBUF *b, const IPHDR *ip);
void  nsficmp_send_error(const PBUF *orig, UCHAR type, UCHAR code);
```

### 11.3 Fragmentation Policy (explicit, previously implicit)

**v1 does not reassemble and does not fragment.**
- Inbound fragments (MF set or nonzero offset) are dropped and counted
  (`ip_fragdrop`). With CTCI MTU 1500 and NSF's small TCP MSS this cannot
  affect NSF-initiated traffic; it only limits exotic inbound senders.
- Outbound: transports respect the device MTU (TCP MSS derivation,
  UDP send fails with EMSGSIZE if datagram > MTU − 28).
- Reassembly is a defined M5+ option with its own memory budget decision —
  reassembly buffers are exactly the kind of unbounded cost Goal 1 forbids
  without explicit sizing. (ADR-0012)

### 11.4 Routing Table

Fixed array (default 16 entries) built from PROFILE `GATEWAY`/`HOME`:
host routes, network routes (classful + mask), default route. Longest
match wins; lookup is a linear scan of ≤16 entries — measurable cost zero
at NSF scale. Read-only after init (VARY-time changes are M5+).

### 11.5 Checksum

One shared routine `in_cksum(PBUF *chain, USHORT offset, USHORT len)` in
portable C first (RFC 1071 algorithm), with an HLASM drop-in later if
profiling justifies it — identical signature (Project Brief §2.2).
Host-build unit tests pin it against known vectors before any packet flies.

### 11.6 ARP (Milestone 6, with LCS)

Separate module NSFARP; cache with NSFTMR-driven expiry; only the
next-hop resolution hook in `nsfip_output` knows it exists. On CTCI the
hook resolves to "the peer" unconditionally.

### 11.7 Statistics (minimum set)

`in, out, hdrerr, badcksum, badlen, noproto, noroute, fragdrop, ttlexp,
icmp_inecho, icmp_outecho, icmp_errsent`.

---

## 12 — UDP (NSFUDP)

### 12.1 Responsibilities

Datagram send/receive, port demultiplex, checksum (computed on output,
verified on input; zero checksum accepted per RFC 768).

### 12.2 Control Block & Demux

```c
typedef struct udppcb {
    QELEM   q;            /* pcb list linkage                 */
    SOCKCB *sock;
    UINT    laddr;        /* 0 = INADDR_ANY                   */
    USHORT  lport;
    USHORT  flags;
} UDPPCB;                 /* pool objsize 64 for growth       */
```

Demux: linear scan of the bound-pcb list (≤32 default) matching
(lport, laddr|ANY). Unmatched → ICMP port unreachable + count.

### 12.3 Data Flow & Lifetime

- Input: PBUF trimmed to payload, enqueued on `sock->rxq` (bounded; full →
  drop + count) with a small prepended address record (src addr/port) for
  RECVFROM; a parked RECV is completed immediately.
- Output: SENDTO copies user data into a PBUF (headroom for UDP+IP),
  builds the UDP header, `nsfip_output()`.
- UDPPCB lifetime: created at bind time, destroyed in `soc_destroy` via
  `detach` callback. No timers, no state machine — UDP is the end-to-end
  proof of the socket/request path (M3) precisely because it is trivial.

---

## 13 — TCP (NSFTCP)

The largest component; this chapter fixes structure and lifetime, while
algorithm details follow RFC 793/1122 directly.

### 13.1 Responsibilities

Full state machine (11 states), sequence processing per RFC 793,
handshake and teardown, sliding-window flow control with small fixed
windows, retransmission (simple fixed RTO in M4 → Karn + exponential
backoff in M5), delayed ACK and Nagle (M5, both configurable), RST
generation/handling, keepalive.

### 13.2 TCB

```c
typedef struct tcb {
    QELEM    q;              /* 8   tcb list                        */
    SOCKCB  *sock;           /* 4                                   */
    UCHAR    state;          /* 1   CLOSED..TIME_WAIT               */
    UCHAR    flags;          /* 1   NODELAY, KEEPALIVE, ...         */
    USHORT   mss;            /* 2                                   */
    /* send sequence space (RFC 793 names) */
    UINT     snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2, iss;
    /* receive sequence space */
    UINT     rcv_nxt, rcv_wnd, irs;
    /* queues */
    QUEUE    sndq;           /* unsent + unacked PBUF chain         */
    UINT     sndq_bytes;
    QUEUE    oooq;           /* out-of-order segs (M5; bounded 4)   */
    /* timers — embedded, never allocated */
    TMR      t_rexmit, t_persist, t_keep, t_2msl;
    /* RTO state */
    USHORT   rto, srtt, rttvar;
    UCHAR    backoff, dupacks;
    USHORT   rsvd;
} TCB;
/* target: 256-byte pool object — SIZE_ASSERT enforced */
```

Window sizes: default `rcv_wnd` 4096, configurable via TCPCONFIG
(RECVBUFRSIZE), bounded by BUFLARGE availability. Small windows are a
feature here, not a deficiency: they are the mechanism by which the memory
budget bounds per-connection buffering.

### 13.3 Input Processing Shape

`tcp_input(PBUF*, IPHDR*)` implements the RFC 793 event-processing
skeleton literally (the "SEGMENT ARRIVES" section), as one function with
per-state handlers — the structure auditors can hold next to the RFC.
Demux: linear TCB list scan on the 4-tuple, then listener match on
(lport, laddr|ANY).

### 13.4 Teardown Checklist (lifetime)

A TCB dies through exactly one function, `tcp_destroy(tcb)`:
1. `tmr_cancel` × 4 (rexmit, persist, keep, 2msl),
2. free `sndq` and `oooq` PBUF chains,
3. unlink from TCB list,
4. detach from SOCKCB (which handles parked NSFRQEs),
5. `mm_free(TCPTCB)`.
TIME_WAIT holds the TCB (nothing else) for 2MSL via `t_2msl`; under
`TCPTCB` pool pressure the oldest TIME_WAIT TCB is reclaimed early —
graceful degradation, counted.

### 13.5 Statistics (minimum set)

`activeopen, passiveopen, established, resetsent, resetrcvd, rexmit,
dupack, badcksum, oooseg, wndprobe, keepdrop, timewaitreclaim`.

---

## 14 — Configuration (NSFCFG)

### 14.1 Responsibilities

Parse a PROFILE.TCPIP-compatible member at startup; validate completely;
reject startup on any error (message NSF7xxE with line number). No partial
configs, no defaults silently covering typos (`WARNING` only for unknown
statements explicitly listed as ignorable).

### 14.2 Supported Statements (v1)

```
DEVICE  devname CTC  cuu            ; LINK link CTC 0 devname
HOME    ipaddr link
GATEWAY (net|DEFAULTNET) firsthop link mtu (subnet mask|0)
PORT    port  (TCP|UDP)  jobname    ; reservations
TCPCONFIG  RECVBUFRSIZE n  SENDBUFRSIZE n  KEEPALIVEOPTIONS ...
UDPCONFIG  ...
NSFPOOL poolname count               ; NSF extension: pool sizing
NSFTRACE comp ON|OFF                 ; NSF extension: startup trace
```

IBM statement syntax is normative where IBM defines it (IP Configuration
Reference); `NSF*` statements are clearly namespaced extensions. Parser is
plain C, host-unit-tested against a corpus of valid and broken profiles.

### 14.3 Output

A single immutable `NSFCFG` structure consumed by component inits.
Read-only after init — no locking, no reload in v1 (operator VARY is a
Phase 2+ feature).

### 14.4 Interface & Structure (M0-7)

Two public functions (`include/nsfcfg.h`), each with an 8-char `asm()` alias
(scheme `NSFCF*`; every parser helper is `static`):

```c
INT cfg_parse(const char *buf, UINT len, NSFCFG *out) asm("NSFCFPRS");
INT cfg_load (const char *name,          NSFCFG *out) asm("NSFCFLDR");
```

- `cfg_parse` is **pure C** over a caller-supplied text buffer — no I/O, fully
  host-testable, charset-transparent. Returns `0` on success or the `NSF7xx`
  code on failure.
- `cfg_load` is a thin wrapper: `fopen`/`fread` `name` (a host path, or an MVS
  DDNAME / `'DSN(MEMBER)'` spec understood by libc370's `fopen`) into a
  file-scope init-window buffer (`NSFCFG_FILE_MAX`, 4 KB — chosen over a large
  stack frame on the executive task), then `cfg_parse`. The MVS PDS text-mode
  line handling is a libc370 concern assumed here; it is exercised on-MVS at
  M0-8, not host-verified.

`NSFCFG` is one fixed-size, pointer-free struct (**1160 B**, host size == target
size): bounded arrays (`NSFCFG_MAX_DEVICES/LINKS/HOMES/GATEWAYS/PORTS/POOLS/
TRACES`) of the per-statement records, the `TCPCONFIG`/`UDPCONFIG` scalars, an
embedded `NSFCFGERR` report, and an `eye` eyecatcher (`"NSFCFG  "`). Every
record carries a `NSF_SIZE_ASSERT`. IPv4 values are stored MSB-first
(`10.1.2.3` → `0x0A010203`) for direct use as a 32-bit network address.

### 14.5 Validation Contract (M0-7)

All-or-nothing (goal 8, spec 14.1): the parser validates the whole member and
rejects on **any** error, rendering an `NSF7xxE` message with the **1-based
line number** into `out->err` (line `0` for a whole-config error such as a
missing `HOME`). The eyecatcher is stamped **only** on success, as the last
act — so "eyecatcher present" ⇔ "fully valid config" and no partial config
survives a rejection. Error codes: `700` syntax · `701` bad IPv4 · `702` bad
mask · `703` bad cuu · `704` value out of range · `705` duplicate · `706`
too-many (array overflow) · `707` missing required · `708` unknown statement ·
`709` unknown keyword · `710` member too large · `711` open failed. An unknown
**keyword** inside a known statement is an error (`709`, no silent default
covering a typo); an unknown **statement** is an error (`708`) **unless** it is
on the explicit ignorable list (`TRANSLATE`, `DATASETPREFIX`, `ARPAGE`,
`INFORM` — recognized but not consumed in v1), in which case it is a counted
warning (`NSFCFG.nwarn`) and parsing continues. `HOME` is the one required
statement in v1.

**Charset transparency (spec 15.3, load-bearing).** The parser compares
character and string *literals* only (`'.'`, `';'`, `"DEVICE"`, …) and folds
case block-wise over the sub-ranges contiguous on **both** EBCDIC and ASCII
(`0-9`; `A-I`/`a-i`, hence `A-F`/`a-f` for hex; `J-R`/`j-r`; `S-Z`/`s-z`). It
never hardcodes a byte value and never assumes full-alphabet collation, so the
identical source parses the ASCII host corpus and a real EBCDIC PDS member.

**Deferred to M0-8 (the first consumer), not the parser.** Cross-statement
referential integrity — a `LINK` naming a defined `DEVICE`, a `HOME`/`GATEWAY`
naming a defined `LINK` — is deliberately not validated here: spec 14.2 is
silent on the ordering/reference rules such a check would require, so it belongs
to the M0-8 startup (feeding pool sizes to NSFMM/NSFBUF, trace flags to NSFTRC,
the interface/routing tables to NSFDEV/NSFIP) or a future ADR. `TCPCONFIG` v1
handles `RECVBUFRSIZE`/`SENDBUFRSIZE`; `KEEPALIVEOPTIONS` (a block form) is
deferred and until implemented is rejected as an unknown keyword.

Host-tested by `test/tstcfg.c` against `test/cfg/` (4 valid profiles exercising
every statement type + cosmetics/ignorable handling, 10 broken profiles — one
error class each, asserting the code and the exact line — plus inline
dotted-decimal edge cases): TSTCFG 111/111.

---

## 15 — EZASOKET API (NSFEZA)

### 15.1 Responsibilities

- Provide the IBM EZASOKET call interface (call-by-name with the IBM
  parameter lists, TOKEN/task-storage semantics, RETCODE/ERRNO pairs)
  for C and HLASM callers.
- Translate each call into an NSFRQE, submit it, WAIT, and map completion
  back into IBM return conventions.

### 15.2 Function Coverage by Milestone

| Milestone | Functions |
|---|---|
| M3 (UDP) | INITAPI, SOCKET, BIND, SENDTO, RECVFROM, CLOSE, TERMAPI, GETSOCKNAME |
| M4 (TCP) | CONNECT, LISTEN, ACCEPT, SEND/WRITE, RECV/READ, SELECT, SHUTDOWN, SETSOCKOPT, GETSOCKOPT, FCNTL/IOCTL(FIONBIO), GETPEERNAME |
| M6 | Completeness pass driven by what HTTPD and mvsMF actually call (audit against Shelby Beach's EZASMI and Jason Winter's x75.c usage); GETHOSTBYNAME et al. with the DNS resolver |

ERRNO values follow the IBM EZASOKET documentation (not Unix `errno.h`
values where they differ). A conformance table lives in `docs/ezasoket-
conformance.md` and is updated per milestone — it is the acceptance
artifact for M6.

### 15.3 Character Set Statement (explicit)

The socket layer is **binary-transparent**, exactly like IBM's: NSF never
converts payload between EBCDIC and ASCII. Conversion is the
application's responsibility (as HTTPD already does). The only EBCDIC↔
ASCII conversion inside NSF is for its own text I/O (config parsing,
operator messages, trace) and later the DNS resolver's name handling.

---

## 16 — Testing (NSFTEST)

### 16.1 Test Pyramid

```
Level 0  Host unit tests        checksum vectors, queue/pool/timer logic,
         (native cc via         TCP state machine against scripted segments,
          make test-host, CI)   PROFILE parser corpus
Level 1  Host integration       full stack on NSFHOST loopback/TUN driver:
         (native cc via         ping, UDP echo, TCP echo, packet-loss
          make test-host, CI)   harness (drop/duplicate/reorder injection)
Level 2  MVS component jobs     per component: pool exercise, timer accuracy
         (MBT test-mvs)         (validates ADR-0011), EXCP smoke
Level 3  MVS integration        ping/UDP echo/TCP echo from the Hercules
         (MBT test-mvs)         host against the STC
Level 4  Acceptance             HTTPD serving pages, mvsMF REST calls, over
         (MBT test-mvs)         NSF instead of X'75'
```

Levels 0–1 are the CI gate on every change; they build with a native
compiler (via `make test-host`) and require no MVS. Levels 2–4 run on a
live MVS 3.8j reachable over IP (via `make test-mvs`, which deploys then
runs the suite), at milestone boundaries and before merges that touch
MVS-specific code. Both are MBT V2 targets (ADR-0014); the distinction is
MVS reachability, not MBT vs. not-MBT.

### 16.2 Repository Layout (enforces host/MVS separation)

The tree is an **MBT V2 project** (`project.toml` at the root; the `Makefile`
is the two-line MBT include). The layout is **flat** — portable C in
`src/*.c`, HLASM in `asm/*.asm` — matching the mvslovers ecosystem
(rexx370, mvsMF, httpd). The `nsf*` filename prefix already namespaces every
source, so no per-layer subdirectories are needed; components stay grouped by
prefix and by the module map in CLAUDE.md §9 (ADR-0014, superseding the
grouped tree of v1.1).

```
nsf370/
├── project.toml      MBT V2: modules, sources, tests, deps, [host] table
├── Makefile          two lines: MBT_ROOT := mbt + include mbt/mk/mbt.mk
├── .env.example      MVS connection template (.env is git-ignored)
├── mbt/              MBT V2 — a git submodule
├── src/              portable C: nsfmm.c nsfbuf.c nsfque.c nsftmr.c
│                     nsfevt.c nsftrc.c nsfsts.c nsfip.c nsficmp.c
│                     nsfudp.c nsftcp.c nsfsoc.c nsfreq.c nsfeza.c
│                     nsfdev.c nsfctci.c … plus nsf*_host.c host shims
├── asm/              HLASM: nsfctci.asm nsfxq.asm nsfstim.asm nsfwto.asm
│                     nsfestae.asm            ← ONLY MVS-specific code
├── include/          one header per component (nsf.h + nsf*.h)
├── cfg/              sample PROFILE.TCPIP members
├── jcl/              install/SAMPLIB jobs (driven by MBT)
├── test/             dual host+MVS tests (tstsmoke.c, …)
│   ├── mvs/          Level 2/3: on-MVS component & integration jobs
│   └── asm/          HLASM test callers
└── docs/
    ├── Project-Brief-v2.md
    ├── Architecture-Specification.md
    └── adr/          ADR-0001 …
```

Two build rules, both enforced in review:
1. `asm/*.asm` never compiles on the host; the `[host].replace` map in
   `project.toml` swaps each MVS-only CSECT for its `src/*_host.c` shim.
   Everything else compiles both ways, warnings-as-errors.
2. Host tests (`make test-host`) use the native compiler and need **no
   MVS**, so CI runs them without an MVS system in the loop. They are still
   an MBT target — there is no separate `host.mk` (ADR-0014). The two worlds
   share the portable protocol sources; only `asm/*.asm` and the host shims
   differ.

### 16.3 Definition of Done (every milestone)

1. All Level 0/1 tests green in CI (native build, no MBT).
2. The milestone's demonstrable deliverable shown on Hercules via
   `make test-mvs` (Level 3).
3. `DISPLAY STATS` after the demo shows zero unexplained drops and all
   pools return to baseline in-use counts after quiesce (leak gate).
4. Documentation updated: this spec's affected chapters + ADRs.

---

## 17 — Recovery & Serviceability (addition beyond the Brief)

### 17.1 ESTAE Coverage

- The executive task establishes an ESTAE at init via **libc370 `__estae`**
  with a **C recovery function `nsf_recover`** (M0-8, ADR-0018 — not a
  hand-rolled `asm/nsfestae.asm`: a raw asm→C recovery bridge re-implements
  `@@estae`'s C-environment setup and is the issue-#8 failure class).
  On ABEND `nsf_recover`: WTO a marker, capture is implicit (the trace ring
  "NSFTRACE", the stats registry "NSFSTATS" and every pool header carry
  eyecatchers and are already in the dump — §17.2), call the **same** teardown
  the orderly path uses (`nsf_shutdown`: disarm the timer exit, free pool
  regions; M1+ adds device quiesce), then **percolate** (`SDWARCDE = SDWACWT`).
  The ESTAE is deleted **first** in shutdown so a teardown fault cannot re-enter
  recovery.
- Phase 1 in-process builds establish ESTAE around the stack subtask so an
  application failure and a stack failure cannot take each other down
  undiagnosed.
- Rationale: on MVS 3.8j, secondary failures during termination (e.g.
  Sx03-class ABENDs from resources still held at EOT) are notoriously
  harder to debug than the primary failure. NSF's teardown-checklist
  discipline (soc_destroy, tcp_destroy, device quiesce) exists so that the
  ESTAE path can call the same functions the orderly path uses.

### 17.2 First-Failure Data Capture

Trace ring with eyecatcher, pool headers with eyecatchers, NSFRQE/PBUF
sequence numbers — a raw dump plus this spec should suffice to diagnose
most failures without a live reproduction.

### 17.3 Security Posture (statement of record)

Phase 1 offers no isolation (library in the caller's space) — acceptable
for a hobbyist system. Phase 2 concentrates the stack in one STC; the SSI/
SVC boundary is where request validation (addresses, lengths, socket
ownership by ASID) becomes a real security surface and is designed as
such in the M5 design review. Unlike X'75', no problem-state program ever
gets raw host-network access.

---

## 18 — Architecture Decision Records (index + summaries)

Each ADR gets its own file under `docs/adr/`; the summaries here are
normative until the files exist.

| ADR | Decision | Essence |
|---|---|---|
| **0001** | Event-driven executive | One event loop maps directly onto MVS WAIT/POST/ECB; run-to-completion removes nearly all locking in Phase 1; protocols become pluggable consumers. Rejected: task-per-connection (memory, serialization), interrupt-driven protocol processing (undebuggable). |
| **0002** | CTCI first | Raw IP point-to-point, no ARP, simplest CCW programming, standard in TK4-/TK5. LCS later proves the device abstraction. |
| **0003** | Phase 1 in-process | Prove protocols where debugging is trivial; the NSFRQE request block is designed transport-neutral from day one, so Phase 2 changes the transport, not the contract. Rejected: CSA-based shared stack from the start (the v1 brief's plan — highest-risk MVS programming before any protocol works). |
| **0004** | Fixed memory pools | 24-bit determinism; exhaustion is a countable, graceful event instead of a random failure. Rejected: general malloc (fragmentation, hidden footprint). |
| **0005** | No Xinu code | Fresh implementation from RFCs/IBM docs; mvs38j-ip and Comer Vol. 2 are reading material. License clarity, cc370 instead of Dignus, and the executive architecture don't fit a port anyway. |
| **0006** | ESTAE + teardown checklists from M0 | Recovery cannot be retrofitted; secondary termination failures on 3.8j are the most expensive bug class. |
| **0007** | Prefix `NSF`, subsystem `NSFS` | Non-colliding with IBM EZA/real z/OS message space; reflects the "Network Services Facility" identity. EZASOKET keeps IBM names at the API boundary. |
| **0008** | Single-owner buffers, no refcounting | Refcounts in C on a memory-starved system create leak/UAF classes; explicit handoff is auditable. Cost: one copy at the app/stack boundary — accepted. |
| **0009** | Two buffer classes (256/2048) | One class wastes ~87% on ACK-sized traffic; more than two classes complicates sizing for no measured gain. Revisit with M4 stats. |
| **0010** | Delta-queue timers, no wheel | Tiny active-timer population; zero fixed memory cost; trivially correct. |
| **0011** | 100 ms tick via STIMER (single interval, re-armed) | Sufficient for RTO ≥ 1 s and 200 ms delayed ACK. 3.8j has STIMER (SVC 47), **not** STIMERM (corrected M0-5). Gate: M0 timer-accuracy job on Hercules (`test/mvs/tsttmacc.c`) — mean ∈ [90,110] ms, no interval > 200 ms — pending a live run before freeze. Full record: `docs/adr/ADR-0011-...md`. |
| **0012** | No IP fragmentation/reassembly in v1 | Reassembly is unbounded memory by nature; explicitly dropped + counted. MSS/EMSGSIZE keep NSF-originated traffic unfragmented. M5+ option with its own budget. |
| **0013** | Toolchain: cc370 + libc370, orchestrated by MBT V2 | cc370 is a complete host cross-compile suite (no c2asm370 assembler round-trip); libc370 is its target C library. MBT V2 is the ecosystem-standard Python build system (project.toml, host cross-compile + on-MVS assemble/link, XMIT packaging) already proven on mvsMF — so NSF inherits a known-good build/dependency/test model instead of a bespoke one. Rejected: GCCMVS/CRENT370 (superseded in the ecosystem), c2asm370 (extra assembler step, not needed with cc370). |
| **0014** | Build model & repo layout follow MBT V2 conventions | Building the M0-1 skeleton against the real ecosystem repos showed v1.1's §1.6/§16.2 assumptions were wrong. MBT V2 drives **both** builds — the host build is a first-class target (`make test-host` + a `[host]` table with a `replace` map for MVS-only CSECTs), **not** a separate `host.mk`. Layout is **flat** (`src/*.c`, `asm/*.asm`), not grouped by layer. `libc370` is the cc370 sysroot, not a `[dependencies]` entry. Real targets are `deps`/`test-host`/`modules`/`package`/`deploy`/`test-mvs` — no `bootstrap`/`build`/`link`. Supersedes §1.6 and §16.1/§16.2/§16.3, corrected inline in this version. Full record: `docs/adr/ADR-0014-build-model-and-repo-layout.md`. |
| **0015** | NSFMM pool regions via libc370 `malloc`, not a raw GETMAIN SVC | The one region-per-pool acquisition (§2.3) uses libc370 `malloc`/`free` from portable C. On 24-bit MVS 3.8j that resolves to `GETMAIN` below the 16 MB line, released at `mm_shutdown` — realizing §2.3's intent while keeping NSFMM pure C (no assembler region helper). Load-bearing consequence: do not "restore" a raw GETMAIN SVC believing §2.3 mandates one. Full record: `docs/adr/ADR-0015-region-acquisition-via-libc370-malloc.md`. |
| **0016** | Shared platform seam `nsftime` (`nsf_now` + `nsf_taskid`) | The two platform facts NSF asks the machine for — a monotonic timestamp and the current task id — live behind one C-callable seam (`include/nsftime.h`): MVS `STCK` + `PSATOLD` in `asm/nsftime.asm`, `gettimeofday` + `0` in `src/nsftime_host.c`, swapped by the `[host].replace` map (the NSFXQ pattern). `nsf_now` is **not** trace-private: NSFTMR reuses it at M0-5. The value's epoch/scale is platform-specific (STCK TOD vs host wall clock), so callers order/relative-time with it but never assume a shared tick unit or wall-clock meaning. First consumer: NSFTRC (§7.2). Full record: `docs/adr/ADR-0016-shared-platform-time-and-task-seam.md`. |
| **0017** | Timer wakeup via the async STIMER REAL exit (not a subtask) | The 100 ms heartbeat that wakes the loop is a `STIMER REAL` **exit** (`NSFTMEXP`) that POSTs the timer ECB and re-arms — one task, no ATTACH, and the *same* async-exit convention M1 device I/O exits reuse. OS-invoked, so no `FUNHEAD`; built to the documented MVS 3.8 STIMER-exit linkage (a hand-rolled shortcut caused an S0C6). Runtime-validated on 3.8j at M0-6. Full record: `docs/adr/ADR-0017-timer-wakeup-async-stimer-exit.md`. |
| **0018** | Operator / WTO / ESTAE reuse libc370 seams (no hand-rolled asm) | M0-8's three MVS surfaces — CIB/QEDIT operator commands, WTO, and ESTAE recovery — reuse the proven libc370 seams (`__gtcom`/`__cibset`/`__cibget`/`__cibdel`, `wto`/`wtof`, `__estae` + a C `nsf_recover` percolating via `SDWARCDE = SDWACWT`), exactly as `ufsd`/`httpd` do. Tie-breaker: §17.1 requires recovery to call the same **C** teardown, and reaching C from a *raw asm* OS-exit re-implements `@@estae`'s C-environment bridge — the issue-#8 failure class the from-scratch `asm/nsfestae.asm` would reintroduce. So `asm/nsfestae.asm` / `asm/nsfwto.asm` are not created; M0-8 adds **zero** new assembler. Bakes in the two CIB traps from `ufsd` (drain unconditionally; delete ESTAE first). Ratified with the maintainer. Full record: `docs/adr/ADR-0018-operator-wto-estae-via-libc370-seams.md`. |

---

## 19 — Roadmap (Fahrplan)

Milestones M0–M6 are the Project Brief's, made concrete as work packages
with entry/exit gates. Order within a milestone is the recommended
implementation order. Effort classes: S (≤ a few sessions), M (a focused
week-equivalent), L (multi-week-equivalent).

### M0 — Foundation ("the skeleton is alive")

| WP | Deliverable | Size |
|---|---|---|
| M0-1 | Repo layout (16.2): MBT V2 `project.toml` + two-line `Makefile`, `mbt` submodule, `.env.example`; `make test-host` (native, via the `[host]` table) and `make test-mvs` green on a live MVS 3.8j, running the TSTSMOKE build-skeleton test; CI green. **Done** (ADR-0014). | M |
| M0-2 | NSFQUE + NSFMM + size-assert discipline, host unit tests (also: `nsf_abend` enforcement primitive; region seam per ADR-0015). **Done.** | M |
| M0-3 | NSFBUF (PBUF, headroom, chains) + tests. **Done.** | S |
| M0-4 | NSFTRC (ring + macros) and NSFSTS (registry); shared `nsftime` seam (`nsf_now`/`nsf_taskid`, ADR-0016). **Done.** | S |
| M0-5 | NSFTMR + host tests; **MVS timer-accuracy job (gate for ADR-0011)**. **Done** (STIMER, not STIMERM; gate met + frozen). | M |
| M0-6 | NSFEVT main loop + xq exit handoff (host: pthread-simulated exit). **Done** (async STIMER exit, ADR-0017; TSTEVT + TSTEVTM). | M |
| M0-7 | NSFCFG parser + profile corpus tests. **Done** (§14.4/14.5; TSTCFG 111/111). | M |
| M0-8 | MVS STC skeleton: init → loop → MODIFY DISPLAY/TRACE/STATS/STOP → clean shutdown; ESTAE established; WTO messages NSF001I…. **Done** (ADR-0018; operator/WTO/ESTAE via libc370 seams; TSTOPR + TSTSTC host, TSTSTCM + `jcl/NSFPROC.jcl` on-MVS). **M0 complete.** | M |

**Exit gate:** STC starts on TK5, answers `F NSF,DISPLAY,STATS`, stops
cleanly with all pools at baseline; CI green. **MET** — validated live on TK5
(mvsdev): `S NSF` → NSF001I; `F NSF,DISPLAY/STATS/TRACE IP ON` all reply, the
DISPLAY reflecting the deployed config and `TRACE FLAGS 0200→0201` proving the
toggle took effect on EBCDIC; `P NSF` → NSF011I → `IEF142I ... COND CODE 0000`
with an empty SYSUDUMP DD (no dump, confirmed from the full job spool). Deployed via `jcl/NSFPROC.jcl` + `cfg/PROFILE`. The induced-ABEND →
percolate path is covered by `test/mvs/tststcm.c` (ESTAE establish/delete) rather
than force-run (a percolate leaves a dump + terminates the address space).

### M1 — CTCI: bits on the wire

| WP | Deliverable | Size |
|---|---|---|
| M1-1 | **Verify CTCI frame format against Hercules `ctc_ctci.c`; write into Ch. 9.3 as normative.** **Done** (byte-exact: 3088 pair, CTCIHDR/CTCISEG, big-endian). | S |
| M1-2 | NSFDEV device table + DEVOPS contract + NSFHOST loopback/TUN driver. **Done** (§9.2/§9.4; the async `doneq → EV_PACKET_RECEIVED` handoff validated over the host loopback driver's pthread reader thread — the CTCI-exit analog; TSTDEV 80/80 host-green). | M |
| M1-3 | HLASM top half: EXCP READ/WRITE CCW chains, I/O exit → xq_push + POST | L |
| M1-4 | C bottom half: frame ↔ PBUF, READ re-drive, sendq kick, MIH idle handling | M |
| M1-5 | Trace hexdump of received packet on console; hand-crafted packet sent | S |

**Exit gate:** ping from the host produces hexdumps in NSF trace (no reply
yet); a crafted packet from NSF is visible in host `tcpdump`.

### M2 — IP + ICMP: "Ping!"

| WP | Deliverable | Size |
|---|---|---|
| M2-1 | `in_cksum` + RFC 1071 vectors (host tests first) | S |
| M2-2 | IP input (validate/demux) + output (build/route) + routing table from PROFILE | M |
| M2-3 | ICMP echo responder (reply in-place in the same PBUF) | S |
| M2-4 | ICMP errors: port/proto unreachable, TTL exceeded | S |
| M2-5 | Stats + trace wired through; fragment-drop counting (ADR-0012) | S |

**Exit gate:** `ping <mvs-ip>` works sustained (1000 packets, 0 loss on
loopback-quality link); stats consistent; pools at baseline afterwards.

### M3 — UDP + socket path end-to-end

| WP | Deliverable | Size |
|---|---|---|
| M3-1 | NSFSOC socket table + PROTOPS + parked-request pattern | M |
| M3-2 | NSFREQ + NSFRQE transport (Phase 1: queue+POST between app task and executive) | M |
| M3-3 | NSFUDP in/out, demux, ICMP port-unreachable hookup | M |
| M3-4 | NSFEZA: M3 function set with IBM parameter lists/ERRNOs | M |
| M3-5 | UDP echo server sample (MVS) + host-side test client | S |

**Exit gate:** UDP echo via EZASOKET API works from host; kill -9 of the
client and socket close paths leak nothing (leak gate).

### M4 — TCP

| WP | Deliverable | Size |
|---|---|---|
| M4-1 | TCB + state machine skeleton, RFC 793 "SEGMENT ARRIVES" structure; host-scripted segment tests (SYN, RST, bad seq…) | L |
| M4-2 | Handshake + teardown incl. TIME_WAIT via NSFTMR | M |
| M4-3 | Data path: send queue, sliding window, in-order receive → rxq | L |
| M4-4 | Retransmission: fixed RTO + basic backoff | M |
| M4-5 | EZASOKET M4 set incl. SELECT; TCP echo server sample | M |
| M4-6 | Host loss-injection harness (drop/dup/reorder) run green | M |

**Exit gate:** telnet from host to TCP echo: interactive session, clean
FIN teardown, survives 5% synthetic loss on host harness; TIME_WAIT
reclaim under pool pressure demonstrated.

### M5 — Subsystem split + hardening (Phase 2)

| WP | Deliverable | Size |
|---|---|---|
| M5-1 | **Design review first:** SSI registration (à la mvslovers/ufsd), SVC vs. SSREQ choice, cross-memory POST, ubuf move (MVCK), request validation/security (17.3), client-death cleanup via owner_ascb | M |
| M5-2 | NSFS subsystem STC + NSFEZA stub transport; applications relink only | L |
| M5-3 | TCP hardening: Karn, exponential backoff, delayed ACK, Nagle, cwnd/slow-start (simplified), out-of-order queue | L |
| M5-4 | NETSTAT-style DISPLAY (connections, per-state counts) | S |
| M5-5 | Memory-pressure degradation tests; multi-connection stress | M |
| M5-6 | Install JCL (SAMPLIB), operator + programmer guides | M |

**Exit gate:** two separate application address spaces share one NSFS
stack concurrently; stress run with N configurable connections; docs
complete.

### M6 (stretch) — Application compatibility + LCS

HTTPD on NSF → mvsMF on NSF → EZASOKET conformance doc complete → DNS
stub resolver → LCS driver + ARP (proving Ch. 9.5's zero-changes-above
claim).

**Final acceptance (project success per Brief §1):** HTTPD and mvsMF run
unchanged (relink only) on the native stack on TK4-/TK5.

### Sequencing Notes

- M0-1/M0-2 are the critical path; everything else in M0 parallelizes.
- The Phase-boundary contract (NSFRQE, Ch. 10.4) freezes at M3 exit;
  changes afterwards need an ADR.
- The only milestone with real schedule risk is M5 (hard MVS systems
  programming); it is deliberately isolated so M0–M4 deliver a usable
  in-process stack even if M5 stalls.

---

## Appendix A — Change Log

**v1.13:** M1-2 (device abstraction + host driver) implemented and host-validated.
§9.2 gains the **device table & loop seam**: `NSFDEV` owns a fixed device table
(`dev_register`/`dev_find`/`dev_find_cuu`/`dev_by_index`/`dev_foreach`/`dev_start`/
`dev_send`/`dev_shutdown`) and registers three hooks with the executive loop
(`evt_set_devices`, mirroring `evt_set_operator`) so the loop stays
**driver-agnostic**: `nsfdev_collect_ecbs` (device ECBs → ECBLIST),
`nsfdev_poll_input` (drain each `doneq` → `EV_PACKET_RECEIVED`, before dispatch,
lost-wakeup-safe ECB clear, drop+count on EVT exhaustion) and `nsfdev_kick_output`
(§5.3 step 5, drain `sendq` → `ops->send`); `nsfevt_wake` kicks output for a send
issued outside a loop pass. The **`NETDEV`** CB is 64 B (`NSF_SIZE_ASSERT`), the
`send`-ownership rule (§9.2) is enforced, and `dev_shutdown` drains `sendq`+`doneq`
for a clean leak gate. §9.4 expands **NSFHOST**: modes `LOOPBACK` (default, CI),
`TUN` (`-DNSFHOST_TUN`), `PCAP` (reserved), and the **async producer** — a reader
**thread** that `xq_push`es a received PBUF onto `doneq` + POSTs the device ECB,
the host analog of the CTCI I/O-completion exit (M1-3 swaps only the producer).
Loopback relays copy-free; NSFMM is touched only on the executive task, so the
threaded host test does not race the pools. Cross-build discipline: `src/nsfhost.c`
(pthread) is host-only; the MVS build compiles the NULL-ops placeholder
`src/nsfhost_plat.c` (no host driver on MVS — use CTCI/LCS), so a portable test
still cross-links. Host suite **408 → 488** (TSTDEV: send→receive cycle, in-order
delivery, bounded `sendq`, DOWN-device reject, leak gate; 80/80 stress-stable);
`-Wall -Wextra -Werror` clean (host + cc370); NSF module + all 16 test modules
cross-link clean, alias scan clean (13 new `NSFD*`/`NSFH*`/`NSFEV*` externals,
unique, ≤8 chars). §9.2/§9.4 and §19 M1-2 updated (**Done**); the CTCI HLASM top
half (M1-3) and frame codec (M1-4) remain the concrete-driver work.

**v1.12:** M1-1 (CTCI wire format) — verified byte-exact against Hercules `ctc_ctci.c` / `ctcadpt.h` and written into §9.3 as normative, replacing the Project Brief's approximate "raw IP, no framing". The device is a 3088 read/write subchannel pair; each block is a `CTCIHDR` (2-byte next-block offset, 0x0000 = last) carrying `CTCISEG` segments (6-byte header: length incl. header, type 0x0800, reserved) + the IP packet; all halfwords big-endian = native S/370 order. Also bumps the version header (left at 1.10 by the v1.11/M0-8 changelog entry).

**v1.11:** M0-8 (MVS STC skeleton) implemented — **M0 complete**. Assembles the
foundation into the `S NSF` started task: config-driven init → the §5.3 executive
loop → `F NSF,*` / `P NSF` → orderly shutdown, under ESTAE. Adds the operator
interface as a **portable dispatcher** (`nsfopr_dispatch`: DISPLAY / STATS /
TRACE comp ON|OFF / STOP / HELP, WTOing `NSF8xx`) over a thin CIB/QEDIT seam; the
loop gains the §5.3 **cibECB slot** via `evt_set_operator`, whose drain runs
**unconditionally** each pass (not gated on the console ECB bit — the
`IEE342I TASK BUSY` startup-CIB trap). Adds the **NSFCFG→component-init wiring**
(`nsf_init_from_cfg`, spec 14.4): NSFTRACE→`nsftrc_flags`, NSFPOOL→buffer-pool
counts (`buf_init_counts`), and the **cross-statement referential integrity**
check deferred from M0-7 (§14.5: LINK→DEVICE, HOME/GATEWAY→LINK, `NSF720/721/722E`);
DEVICE/routing tables are validated + staged, fed downstream at M1/M2. Recovery
(§17.1) and WTO (`nsfmsg`) reuse the **libc370 seams** (`__estae` + a C
`nsf_recover`; `wto`), not hand-rolled assembler — **ADR-0018**, ratified with the
maintainer; `asm/nsfestae.asm` / `asm/nsfwto.asm` are not created (issue-#8
lesson: a raw asm→C recovery bridge re-implements `@@estae` and breaks the
C-runtime). M0-8 adds **zero** new assembler. Host suite **354 → 408**
(TSTOPR + TSTSTC); cross build (NSF module + all tests) links clean, alias scan
clean; `test/mvs/tststcm.c` + `jcl/NSFPROC.jcl` + `cfg/PROFILE` stage the on-MVS
run. §5.3/§5.4, §17.1 and the §18 ADR index updated; §19 M0-8 marked **Done**.

**v1.10:** M0-7 (NSFCFG configuration parser) implemented and host-validated.
§14 gains the concrete interface and contract: the two public functions
`cfg_parse` (pure C over a buffer) / `cfg_load` (fopen/fread wrapper), the
immutable fixed-size **1160 B** `NSFCFG` output struct (bounded per-statement
arrays with `NSF_SIZE_ASSERT`s, MSB-first IPv4 encoding, embedded `NSFCFGERR`),
and the all-or-nothing validation contract (§14.5): reject on any error with an
`NSF7xxE` message + 1-based line number, no partial config (eyecatcher stamped
only on success), the `700–711` code set, unknown-keyword-vs-unknown-statement
handling, the explicit ignorable list (warn + continue), and `HOME` as the one
required statement. Records the **charset-transparency** mechanism (literal-only
comparison + block-wise EBCDIC/ASCII-safe case fold, no hardcoded byte values —
spec 15.3) that lets the same source parse the ASCII host corpus and an EBCDIC
PDS member. Cross-statement referential integrity and `TCPCONFIG
KEEPALIVEOPTIONS` are explicitly **deferred to M0-8** / a future ADR (spec 14.2
is silent on the rules they need). No new ADR (sits under existing goals 1/6/8);
no control-block-size or milestone-contract change elsewhere. TSTCFG 111/111
over a 14-file `test/cfg/` corpus; 354/354 host suite; §19 M0-7 marked **Done**.

**v1.0:** Initial Architecture Specification, created per the v2
architecture review's recommendation to freeze the Project Brief and
split off a technical blueprint. Adds beyond the Brief: Architecture
Goals (Ch. 00), resolved naming (NSF/NSFS), two-class buffer decision,
delta-queue timer design with 100 ms tick, explicit no-fragmentation
policy, the NSFRQE phase-boundary contract, generation-counted socket
descriptors, teardown-checklist discipline, Recovery & Serviceability
chapter (ESTAE, FFDC), ADR index 0001–0012, and the work-package roadmap.

**v1.1:** Toolchain correction. Adopted **cc370 + libc370** (complete host
cross-compile suite) in place of GCCMVS/CRENT370, and dropped the c2asm370
assembler round-trip. Made **MBT V2 (MVS Build Tools)** the build
orchestrator and recorded its impact on repository shape (`project.toml`,
`.env`) and the test model (host Level 0/1 outside MBT for CI; MBT-driven
Level 2–4 on a live MVS). Added §1.6 Build Toolchain & Environment,
ADR-0013, and updated §16.1/§16.2 and work package M0-1 accordingly.

**v1.9:** M0-6 (NSFEVT event dispatcher / executive main loop) implemented and
validated. The §5.3 loop is realized in `src/nsfevt.c`: WAIT on the ECB list
unless there is pending work, drain the NSFXQ handoff (LIFO) into the event queue
(FIFO), dispatch each event to its registered handler under a **64-event drain
budget** so an `evt_post` flood cannot starve the timer, run `nsftmr_run`, then a
kick-output stub; plus the §5.4 shutdown skeleton (each step stubbed at M0-6
except the timer disarm and freeing pending events for the leak gate). `EVT` is
24 bytes, carved from an NSFMM pool. The WAIT/POST is a platform seam
(`nsfevt_plat_wait`/`_post`): libc370 `ecb_waitlist` (WAIT ECBLIST) on MVS,
a pthread mutex + condition variable host shim, swapped by `[host].replace`.
**New: ADR-0017** — timer wakeup via the **async STIMER REAL exit** (chosen over
a timer subtask: one task, no cross-task POST, and M1 device I/O exits are the
same async class). `asm/nsfstim.asm` `NSFTMEXP` is corrected to the documented
MVS 3.8 STIMER-exit linkage (GC28-0683; the 7-step entry-convention contract is
recorded in ADR-0017 so it is not re-derived) and is now **runtime-validated**:
host `test/tstevt.c` 17/17 (FIFO dispatch, the drain budget, a pthread-simulated
exit driving the xq handoff, the shutdown leak gate); on-MVS `test/mvs/tstevtm.c`
= CC 0, 10 heartbeats at mean 100.2 ms, clean shutdown -- the M0-5 S0C6 in the
exit-dispatch path is gone. Out of scope (unchanged): NSFCFG (M0-7); the STC
skeleton / MODIFY / ESTAE / WTO (M0-8); devices / sockets (M1+).

**v1.8:** Issue #8 fixed and the ADR-0011 gate frozen. The hand-rolled
C-callable HLASM seams (`asm/nsftime.asm`, `asm/nsfxq.asm`, and the C-callable
entries of `asm/nsfstim.asm`, plus `test/asm/tststmw.asm`) are rebuilt on the
**standard cc370 entry convention** — `COPY MVSMACS` + `COPY PDPTOP`, `FUNHEAD`
prologue / `FUNEXIT` epilogue, modeled on libc370 `@@getclk.asm`. Hand-rolled
`STM`/`BALR`/`USING` seams omitted the `ENTRY` / name eyecatcher / `LR R12,R15`
base the cc370 C-runtime path (`@@CRTGET`) relies on and ABENDed the next C call
on MVS (S0C6) — a general mainline-runtime blocker (issue #8), proven by staged
isolation and now a documented invariant (§3, "C-callable HLASM"). The async
`STIMER`-exit `NSFTMEXP` stays hand-rolled (OS-invoked, not a C callee) and
deferred to M0-6. `nsftime` graduates to **runtime-validated** (stage-2 isolation
`nsf_now` + `nsf_taskid` = CC 0; PSATOLD fetch proven); `nsfxq`/`nsfstim` keep
deferred-runtime status (entry convention fixed only). **ADR-0011 is now FROZEN:**
the on-MVS accuracy job measured mean 100.1/100.2 ms, min/max 100 ms, jitter 0 ms
over N=100 — both criteria pass. Preserved: PR #7 aliases, the #5 RS-format `D(B)`
rule, the S102 explicit-displacement addressing, and the column-71 rule.

**v1.7:** M0-5 (NSFTMR) implementation notes folded into §6, plus a corrected
timer-driver decision. NSFTMR: the §6.3 interface gains `nsftmr_init` and the
`nsftmr_count` / `nsftmr_peek` inspection seam, and `nsftmr_run` takes the
elapsed tick count (`nsftmr_run(UINT ticks)`) so the host can inject ticks with
no STIMER; §5.3's main loop passes the elapsed count. The sorted delta queue
(ADR-0010) is implemented on `QELEM` with cancel-hands-delta-to-successor /
fire-consumes-delta bookkeeping and callback-safe firing. **Corrected: ADR-0011**
— split into `docs/adr/ADR-0011-100ms-tick-via-stimer.md` and changed from
`STIMERM` to a single **`STIMER`** (SVC 47) re-armed to the head delta: 3.8j has
no `STIMERM` (an MVS/SP macro absent from the 3.8j macro library), so the
`STIMERM` phrasing in §6/§18, ADR-0016 and the Brief is superseded. New platform
seam `nsfstim.h` (`nsftmr_plat_arm`/`_disarm`/`_ecb`, STIMER + POST exit on MVS,
recording no-op on the host), wired through `[host].replace` like NSFXQ/NSFTIME.
The ADR-0011 accuracy gate (`test/mvs/tsttmacc.c`: mean ∈ [90,110] ms, no
interval > 200 ms) builds and cross-links but is **blocked from running on MVS by
issue #8**, an M0-6 prerequisite: a live `make test-mvs` staged isolation showed
that merely linking `asm/nsftime.asm` (`nsf_now`) breaks the cc370 mainline
C-runtime call path (ABEND S0C6, `CLIBCRT`) — the STCK itself runs; this is a
general mainline-runtime blocker (affects NSFTRC and M0-6's NSFEVT), **not** the
timer logic and **not** a deferrable accuracy gate. The seam run also fixed a real
`asm/nsfstim.asm` bug (S102, ECB addressed at base 0; two as370 traps documented:
no USING for data across entry points → explicit displacement/register-form
addressing, and column-72 continuation). So the 100 ms tick is not yet frozen. No
control-block-size or milestone-contract change beyond adding the 24-byte `TMR`.
All new external functions carry `asm()` aliases (§3, NSFTM* / NSFWAIT).

**v1.6:** External-symbol alias convention (build-linkage correctness, no
behaviour change). cc370/ld370 truncate external names to 8 characters (upcased,
`_` → `@`), which had silently collided three `nsfbuf` pairs (`BUF@TRIM`,
`BUF@COPY`, `BUF@CHAI`) and `nsf_abend`/`nsf_abend_sethook` (`NSF@ABEN`) into one
symbol each — an MVS-only wrong-function dispatch that host tests (no 8-char
limit) cannot see. Every cross-module NSF C function now pins a unique 8-char
`asm()` alias in its header (per-component scheme
`NSFB*`/`NSFM*`/`NSFQ*`/`NSFTR*`/`NSFST*`/`NSFA*`/`NSFX*`, plus `NSFNOW`/
`NSFTASK` for the time seam); the C↔asm boundary CSECTs in `asm/nsfxq.asm` and
`asm/nsftime.asm` are renamed to match their aliases
(`NSFXINIT`/`NSFXPUSH`/`NSFXDRAN`, `NSFNOW`/`NSFTASK`), replacing reliance on
cc370's `@`-mangling. Recorded as a §3 invariant ("External symbols") with an
assembler/C reviewer checklist; no ADR (a build-mechanics convention, not a
design decision), and no control-block, size, or milestone-contract change.

**v1.5:** M0-4 (NSFTRC + NSFSTS) implementation notes folded into §7.2/§8.2,
plus a new shared platform seam. NSFTRC: the §7.2 interface gains
`nsftrc_init`/`enable`/`disable` and the `nsftrc_count`/`peek`/`ring_base`
inspection seam (reused by the M0-8 operator dump); the `TRC` macro adopts the
`, ##__VA_ARGS__` form so flag-only calls compile; `TRCENT` is fixed at 128
bytes, pointer-free, single-writer at M0-4. NSFSTS: the §8.2 interface gains
`sts_init`/`reset`/`count`/`render`; `STSCTR` stays 16 bytes with the component
carried in a wrapping registry record. **New: ADR-0016** — the `nsftime`
platform seam (`nsf_now` via `STCK`/`gettimeofday`, `nsf_taskid` via
`PSATOLD`/`0`), a shared primitive NSFTMR reuses at M0-5, wired through the
`[host].replace` map like NSFXQ. No control-block-size or milestone-contract
change beyond adding the `TRCENT`/`STSCTR`/`NSFTIME` blocks; M0-4 marked
**Done** in the §19 roadmap.

**v1.4:** M0-3 (NSFBUF) code-review rework, §3.2 note only. `buf_trim_tail`
now trims the packet's **logical tail** (the last chain element), not the
buffer it is handed — the earlier behaviour silently dropped bytes from the
middle of a chained packet and the `chainlen` self-check could not detect it.
`buf_init` gains a **return code** (0 ok / non-zero if a pool could not be
created) so the M0-8 executive startup can refuse to start rather than run with
NULL pools. No new ADR (both sit under ADR-0008/0009); no control-block, size,
or milestone-contract change.

**v1.3:** M0-3 (NSFBUF) implementation notes folded into §3.2/§3.3. Added
`buf_init` (init-window pool creation) and `buf_reset_rx` (the M1 inbound
seam) to the §3.2 interface, with a note recording the fixed layout constants
(`NSFBUF_SMALL_DATA` 256, `NSFBUF_LARGE_DATA` 2048, `NSFBUF_HEADROOM` 64), the
192/193 class-selection boundary, and the `sizeof(PBUF) + B` pooled-object
sizing. Clarified the §3.3 `size` field as the forward capacity from `data`
(`(start+B) − data`), which fixes the prepend/trim bookkeeping. No new ADR:
these sit under ADR-0008 (single-owner buffers) and ADR-0009 (two classes).
**No change to any control block size, invariant, or milestone contract.**
Marked M0-3 **Done** in the §19 roadmap.

**v1.2:** Build-model reconciliation with **ADR-0014**, after the M0-1
skeleton was built against the real MBT V2 ecosystem repos. Corrected §1.6
and §16.1/§16.2/§16.3 to the real MBT V2 target set
(`deps`/`test-host`/`modules`/`package`/`deploy`/`test-mvs`; there is no
`bootstrap`/`build`/`link`); recorded that MBT drives **both** builds — the
host build via `make test-host` + the `[host]` table, replacing the separate
`host.mk` of v1.1; adopted the **flat** repository layout (`src/*.c`,
`asm/*.asm`) in place of the grouped tree; clarified that `libc370` is the
cc370 sysroot, not a `[dependencies]` entry. Added ADR-0014 to the §18 index
and fixed the Project Brief filename reference in the header. **No change to
any component interface, data structure, invariant, or milestone** — this
version only aligns the build/layout narrative with what the repository
already implements.
