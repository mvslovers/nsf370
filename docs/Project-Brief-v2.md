# MVS 3.8j Native TCP/IP Stack — Project Brief for Claude Code

*Version 2 — revised after external architecture review.*
*Toolchain references reconciled to cc370 / libc370 / MBT V2 per ADR-0013 (2026-07-05); otherwise unchanged from the frozen v2.*

## Executive Summary

This document describes the development of a **native TCP/IP stack for IBM MVS 3.8j**
running on the Hercules S/370 emulator. The goal is to provide MVS 3.8j (as used in
TK4- and TK5 distributions) with a real, architecture-clean TCP/IP implementation —
as opposed to the existing Hercules X'75' instruction hack that passes socket calls
through to the host OS.

This is an experimental, community-oriented project driven by curiosity and the
challenge of implementing a modern protocol stack on a 1970s/80s operating system.
The project owner is an experienced mainframe developer with deep knowledge of
MVS internals, HLASM, C on z/OS, and the MVS 3.8j hobbyist ecosystem.

**Core design principle (post-review):** Treat this less as "a TCP/IP stack" and more
as a small **network executive** — an event-driven system whose core is a dispatcher
and a set of infrastructure services (memory, buffers, timers, tracing, statistics).
IP, TCP, UDP and ICMP are *consumers* of events, not the center of the design.

---

## 1. Background & Context

### 1.1 What is MVS 3.8j?

MVS 3.8j is the last public-domain release of IBM's MVS operating system (1981).
It runs in S/370 mode on the Hercules emulator, providing:

- **24-bit addressing** — 16 MB virtual address space per address space
- **Big Endian** storage (S/370)
- **EBCDIC** character encoding (network protocols use ASCII — conversion required)
- S/370 instruction set (no 31-bit mode, no ESA/XA features)
- Classic MVS facilities: JES2, VTAM, TSO, batch, subsystem interface, APF authorization
- Hercules provides CTC (Channel-to-Channel) and LCS device emulation for network I/O

The active hobbyist community maintains TK4- and TK5, turnkey MVS 3.8j
distributions with modern enhancements (RAKF security, BREXX, various compilers).

### 1.2 Current TCP/IP in MVS 3.8j

There is **no native TCP/IP stack** in MVS 3.8j. The community uses:

1. **Jason Winter's X'75' instruction** (dyn75): A custom Hercules opcode that maps
   guest-side socket calls directly to the host OS IP stack. This is what TK4-/TK5
   ship with. Shelby Beach's EZASMI provides an EZASOKET-compatible assembler API
   on top of it. Advantages: it works. Disadvantages: it's a Hercules-specific hack,
   security-questionable (every problem-state program gets full host IP access), and
   could never run on real hardware.

2. **Jim Morrison's mvs38j-ip** (2002–2003): An attempt to port the Xinu TCP/IP
   stack to run natively in MVS. The project barely got past the research phase —
   ICMP ping packets could enter the stack, but TCP/UDP/ARP and any API were missing.
   The code is preserved at https://github.com/mvslovers/mvs38j-ip. It used the
   commercial Dignus Systems/C compiler.

### 1.3 Why a New Attempt?

- **Architectural cleanliness**: A real stack vs. an emulator passthrough
- **Community value**: Open source, buildable with free tools, documented
- **Learning project**: TCP/IP internals on the most constrained platform imaginable
- **IBM compatibility**: If the API matches EZASOKET, existing MVS 3.8j network
  applications (HTTPD, FTP, mvsMF) could potentially work with the real stack
- **AI-assisted development**: Claude Code can help systematically implement
  well-documented protocols, which was not available when Morrison tried in 2002

### 1.4 Relationship to the Existing mvs38j-ip Code

The historical `mvs38j-ip` repository (now under the **mvslovers** organization)
should be treated as a **historical reference and proof of concept**, not as the
architectural foundation. It contains valuable ideas and experience, but it was
built under different constraints (Dignus C, Xinu-based, early Hercules). Today's
ecosystem — cc370, libc370, MBT V2 build tooling, HTTPD, mvsMF, Git
workflows — justifies designing a fresh architecture while **reusing ideas, not code**.

---

## 2. Critical Constraints

### 2.1 Memory — The 24-Bit Wall

This is the single most important constraint of the entire project.

**Every design decision must be evaluated against memory impact.**

- Each MVS address space has 16 MB of virtual storage (24-bit addresses: 0–FFFFFF)
- A significant portion is consumed by MVS itself:
  - Nucleus (low storage): ~64K–200K
  - SQA (System Queue Area): shared, limited
  - PLPA (Pageable Link Pack Area): shared read-only modules
  - CSA (Common Service Area): shared read-write, precious
  - The private area (for our code) is what's left — roughly 5–9 MB depending
    on the installation
- There is NO virtual storage above 16 MB (no 31-bit "bar")

**Implications for the TCP/IP stack:**

- Buffer pools must be small and carefully managed (no casual malloc)
- We cannot keep thousands of connection control blocks around
- Packet buffers should be fixed-size, pre-allocated, and recycled
- The TCP window size will be small by modern standards — that's fine
- Data structures must be compact; every byte counts
- Code size matters: each module's footprint is real cost

**Memory-aware design principles:**

- Pre-allocate fixed-size buffer pools at initialization (no dynamic allocation in hot paths)
- Use a dedicated memory manager / slab allocator rather than general-purpose malloc
- **Protocol code never calls GETMAIN/malloc directly** — it requests objects from
  the memory manager, which owns all pools
- Keep control block structures compact; **arrange fields to minimize padding**
  rather than relying on generic `packed` attributes (more portable, and matters
  for host-based testing)
- Buffer and pool sizes configurable via PROFILE.TCPIP
- Monitor and report memory usage: high-water marks, exhaustion detection, leak
  detection (allocation counters per pool)
- Plan for graceful degradation when memory is tight (reject new connections
  rather than crash)

### 2.2 Language Strategy: C and Assembler

The primary implementation language is **C** (compiled with cc370 + libc370).
However, certain components will require or benefit from **HLASM (S/370 assembler)**:

**Must be in Assembler:**
- SVC routine (only in Phase 2, if we implement one for cross-address-space IPC)
- I/O request handling (EXCP/CCW chains for CTCI device communication)
- Interrupt handling / exit routines (I/O completion, attention)
- Cross-memory communication (Phase 2, if using multiple address spaces)
- Any code that runs disabled or in SRB mode

**Should be in Assembler (for space/performance):**
- Checksum calculation (IP/TCP/UDP checksums — tight inner loops)
- EBCDIC ↔ ASCII translation (TR instruction, extremely fast)
- Buffer pool management primitives (GET/FREE from pool)
- Timer services integration (MVS STIMER/STIMERM/TTIMER)

**Should be in C:**
- Event dispatcher and main loop
- Protocol state machines (TCP, IP, ICMP, UDP, ARP)
- Configuration parsing
- Socket API implementation
- Routing table management
- Statistics, tracing, DNS resolver

**Strategy:** Start with everything in C. Profile and identify hot paths.
Rewrite specific modules in HLASM when memory pressure or performance requires it.
All assembler modules must have clean C-callable interfaces (standard linkage conventions).

### 2.3 IBM Compatibility — Stay Close to z/OS

The project should follow IBM's TCP/IP conventions wherever practical, both for
familiarity and for application compatibility:

**API — EZASOKET / EZASMI:**
- The socket API should be compatible with IBM's EZASOKET (C/assembler callable)
- This is the same API that Shelby Beach's EZASMI implements on top of X'75'
- If we match this interface, existing applications can switch from the X'75' hack
  to the real stack without code changes
- Key functions: SOCKET, BIND, LISTEN, ACCEPT, CONNECT, SEND, RECV, SELECT,
  CLOSE, SETSOCKOPT, GETSOCKOPT, etc.
- Parameter lists and return codes should match IBM documentation

**Configuration — PROFILE.TCPIP style:**
- Network configuration should follow IBM's PROFILE.TCPIP conventions
- Users familiar with z/OS TCP/IP should feel at home
- Key configuration items:
  - DEVICE / LINK / HOME (interface definition and IP assignment)
  - GATEWAY (static routing)
  - PORT (port reservations)
  - TCPCONFIG / UDPCONFIG (protocol parameters: window size, keepalive, etc.)
  - Pool sizes and trace flags (our extensions)
- The configuration lives in a PDS member (e.g., SYS1.TCPPARMS(PROFILE))

**Operator Interface:**
- Console commands for status and control (similar to z/OS NETSTAT)
- WTO messages with IBM-style message IDs
- MODIFY command handlers for DISPLAY, TRACE, STATS, STOP

**Deployment Model (see section 3.9 for detail):**
- The long-term target is a long-running started task (STC) that owns the stack,
  registered as an **MVS subsystem** (like z/OS does, and like mvslovers/ufsd).
- But this is deferred: Phase 1 runs in-process as a linked library for fast
  iteration. Phase 2 moves the stack into its own subsystem address space.

### 2.4 Build Environment

- **C Compiler:** cc370 (free, community-maintained; complete host cross-compile suite)
  - Cross-compiles on Linux/macOS to S/370 object code
  - Reason: Dignus Systems/C is technically superior but commercial;
    using cc370 ensures anyone in the community can build and contribute
- **C Runtime:** libc370 (the cc370 target C library: headers, libc.a, crt0/crt1/crtm startfiles, SYS1/crent macros)
- **Assembler:** IFOX00 (free with MVS 3.8j) for HLASM modules
- **Linkage Editor:** Standard MVS Linkage Editor (IEWL)
- **Build System:** MBT V2 (MVS Build Tools) — project.toml drives host cross-compile + on-MVS assemble/link (bootstrap/build/link/package); separate host build for unit tests (see 3.10)
- **Source Control:** Git (GitHub, mvslovers organization)
- **Target System:** MVS 3.8j on Hercules (TK4- or TK5 distribution)

### 2.5 Network Interface

Hercules provides several network device emulations:

- **CTCI (Channel-to-Channel):** Point-to-point IP tunnel via TUN/TAP.
  Emulates a 3088 device. Most straightforward — raw IP packets go in and out,
  no Ethernet framing. **No ARP needed** (point-to-point).
- **LCS (LAN Channel Station):** Emulates an OSA-like adapter with Ethernet
  framing. More complex (requires ARP) but closer to real hardware.

**Decision:** Start with **CTCI**. LCS becomes the second driver behind the same
device abstraction (see 3.6), which is exactly why that abstraction exists.

---

## 3. Architecture

This section was substantially expanded after the architecture review. The guiding
idea: **build the infrastructure first, make protocols pluggable consumers of events.**

### 3.1 Design Philosophy: A Network Executive

Rather than centering the design on TCP, center it on:

1. An **event dispatcher** (the main loop / cooperative scheduler)
2. A set of **foundation services** every layer depends on

TCP, UDP, ICMP and IP are event consumers. This makes the system easier to test,
extend (new protocols, new drivers), and reason about — and it maps naturally onto
MVS's WAIT/POST/ECB model.

### 3.2 Layered Architecture with Strict Interfaces

```
        Applications (HTTPD, FTP, mvsMF, user programs)
                 │
        EZASOKET Library / API        ← IBM-compatible socket interface
                 │
        Request Manager               ← marshals API calls into stack requests
                 │
        Socket Manager                ← protocol-independent socket objects
                 │
        TCP  /  UDP                   ← transport protocols
                 │
        IPv4  (+ ICMP)                ← network layer
                 │
        Device Abstraction Layer      ← common driver interface
                 │
        CTCI  /  LCS  /  future        ← concrete device drivers
                 │
        ── Foundation services, used by all layers ──
        Memory Manager · Buffer Manager · Queue Library ·
        Timer Manager · Event Dispatcher · Trace Facility · Statistics
```

**Fundamental rule:** Every component communicates only through well-defined
interfaces. Protocol layers never directly touch device drivers; they go through the
device abstraction. Layers never allocate raw memory; they go through the memory /
buffer managers. This discipline is what makes later extension (LCS, new protocols,
Phase 2 subsystem split) tractable.

### 3.3 Event Dispatcher / Main Loop

The heart of the stack is a single event loop. Conceptually:

```
initialize()            /* config, pools, timers, device, stats, trace */
loop forever:
    wait for any event  /* MVS WAIT on a list of ECBs + timer */
    dispatch event:
        PACKET_RECEIVED   → device → IP input → TCP/UDP/ICMP
        TIMER_EXPIRED     → timer manager → fire callback (retransmit, etc.)
        SOCKET_REQUEST    → request manager → socket manager → protocol
        OPERATOR_COMMAND  → command handler (DISPLAY/TRACE/STATS/STOP)
        SHUTDOWN          → drain, free pools, terminate
    send any queued outbound packets
```

Because event handling runs to completion (packet → process → done,
timer → process → done), the dispatcher **is** the scheduler. No separate
task scheduler is needed. This cooperative model also sidesteps most locking
in Phase 1 (single task, single loop — see 3.8).

### 3.4 Foundation Components (build these first)

Recommended implementation order, before any protocol logic:

1. **Memory Manager** — owns all storage. Fixed pools of predefined object sizes,
   allocated at init. Deterministic usage, leak detection, high-water stats,
   exhaustion detection. Protocol code never calls GETMAIN directly.
2. **Buffer Manager** — packet buffers (mbuf-like, see 3.5). Fixed-size, recycled.
3. **Queue Library** — generic linked-queue primitives (enqueue/dequeue, used for
   buffers, timers, socket backlogs, request queues). One correct implementation,
   reused everywhere.
4. **Timer Manager** — see 3.7. Needed early because so much depends on it.
5. **Event Dispatcher** — the main loop and ECB management.
6. **Trace Facility** — see 3.11.
7. **Statistics Manager** — see 3.12.

Once these exist, the protocol implementation becomes much simpler and each protocol
gets tracing, statistics and timers "for free."

> **Pragmatic note:** Don't build all seven in complete isolation before seeing a
> single packet. Build them to a *minimum viable* state, then bring up ICMP (Milestone
> 2) on top — this keeps momentum and validates the infrastructure against real use.
> Harden the foundation as the protocols reveal what they actually need.

### 3.5 Buffer Ownership Model (mbuf-like)

To minimize copying in a 16 MB address space, packet buffers have a **single owner**
that moves through the stack, rather than being copied at each layer:

```
Driver → IP → TCP → Socket → Application     (inbound)
Application → Socket → TCP → IP → Driver     (outbound)
```

- Only one component owns a buffer at any instant.
- Layers pass buffer *objects* (with headroom for prepending headers), not copies.
- Internal interfaces operate on already-existing buffers; send routines do not
  allocate — the caller provides the buffer.
- This resembles BSD mbufs but is deliberately simpler (fixed-size, chained only
  when necessary).

### 3.6 Device Abstraction Layer

Higher layers must not know whether packets travel over CTCI, LCS, or something else.
Every driver implements a common interface:

```c
int  device_init(struct netdev *dev, struct devcfg *cfg);
int  device_send(struct netdev *dev, struct pbuf *buf);
int  device_receive(struct netdev *dev, struct pbuf **buf);   /* or callback */
int  device_shutdown(struct netdev *dev);
```

The `netdev` structure carries function pointers, device address (CUU), state, and
statistics. Adding LCS later means implementing this interface once — no changes to IP
or above.

### 3.7 Timer Manager

A reusable timer subsystem, implemented in the foundation phase because so many later
components depend on it:

- TCP retransmission, delayed ACK, keepalive, TIME_WAIT
- ARP cache expiry (LCS phase), DNS timeouts

Design: a timer wheel or sorted timer queue, driven by a single MVS STIMERM/TTIMER.
The event loop receives a TIMER_EXPIRED event and the timer manager fires the due
callbacks. Building this once avoids duplicated ad-hoc timer logic in every protocol.

### 3.8 Synchronization

**Phase 1 (single address space, single task, single event loop):** the cooperative
run-to-completion model means almost no locking is required — the event loop processes
one event at a time. The main exceptions are structures touched by **asynchronous I/O
interrupt/exit routines** (the device driver's completion path), which must be
coordinated with the mainline via ECBs and, where needed, CS/CDS (compare-and-swap)
or brief disablement.

Structures whose access discipline must be explicitly documented:

- Buffer Pool (mainline + I/O exit)
- Device I/O queues (mainline + I/O exit)
- Socket Table
- Timer Queue (mainline + timer exit)
- Routing / Interface tables (mostly read-only after init)
- (Phase 2 only) the cross-address-space request queue

**Phase 2 (subsystem address space):** cross-memory access and multiple client tasks
introduce real serialization requirements — designed then, not now.

### 3.9 Deployment Model & IPC — Two Phases

There are two separate questions that were previously conflated:
*(a) where does the stack code run*, and *(b) how do applications talk to it*.

**Phase 1 — In-Process Library (Milestones 0–4):**
- The stack is a set of modules linked directly into the application.
- EZASOKET calls are ordinary C function calls — no IPC, no cross-memory, no SVC.
- Fastest path to a working stack; trivial to debug (one address space, one dump).
- Perfect for bring-up and for the echo-server test programs.

**Phase 2 — Subsystem Address Space (Milestone 5+):**
- The stack moves into its own long-running STC, registered as an **MVS subsystem**
  (IEFSSNxx / dynamic SSI), architecturally similar to **mvslovers/ufsd**.
- One stack instance serves all client address spaces; central configuration and a
  single operator interface — the z/OS model.
- Client applications reach the stack via the subsystem interface (SSREQ) and/or a
  dedicated SVC, with cross-memory POST for completion.
- Crucially, the **EZASOKET library becomes a thin stub**: it forwards requests to
  the subsystem instead of calling the stack directly. Applications are recompiled
  against the same API and **do not change**.

This staging lets us prove the protocol logic in the easy environment first, then
tackle the genuinely hard MVS systems programming (SSI, cross-memory, SVC,
serialization) as a focused, separate effort once the stack itself is trustworthy.

### 3.10 Host-Based Testing

Modern tooling allows most protocol logic to be verified off-MVS:

```
Linux / macOS → native compiler → unit tests → RFC test cases
              → then: S/370 build → integration testing on Hercules
```

Protocol state machines, checksums, parsing, timer logic and buffer management are
plain C and can be tested with a native compiler before ever running under MVS. The
device abstraction (3.6) allows a **loopback/pcap "host" driver** for testing the
full stack on Linux. Keep protocol code free of MVS-specific dependencies so it
compiles both ways.

### 3.11 Trace Facility

A configurable tracing system, part of the foundation (not bolted on later):

- Per-subsystem trace flags: `TRACE IP`, `TRACE TCP`, `TRACE UDP`, `TRACE DRIVER`,
  `TRACE MEMORY`, `TRACE TIMER`
- Controlled at runtime via operator MODIFY command and at startup via PROFILE.TCPIP
- Cheap when disabled (a flag test); formatted output only when enabled
- Packet hex-dump capability for the driver and IP layers

### 3.12 Statistics

Counters maintained from the very beginning — nearly free to add, invaluable for
debugging:

- Packets received / sent (per layer and per device)
- Retransmissions, duplicate ACKs
- Checksum errors (IP/TCP/UDP)
- Buffer allocations / frees / exhaustion events, high-water marks
- Memory usage per pool
- TCP opens / closes / resets, connections by state
- Dropped packets by reason

Exposed via a NETSTAT-like operator command.

### 3.13 Byte Order Convention

S/370 is Big Endian, so network byte order matches host byte order and conversions
are no-ops. **Nevertheless, use `htons()`/`htonl()`/`ntohs()`/`ntohl()` wrappers (or
project macros) everywhere they logically belong.** This documents intent, prevents
bugs if code is ever reused elsewhere, and — importantly — keeps the host-based test
builds (3.10) correct on little-endian machines.

### 3.14 Socket Layer Genericity

Sockets are protocol-independent at the core. A socket object holds only: protocol,
state, buffers, and protocol callbacks. TCP and UDP attach their own logic via those
callbacks. This keeps the socket manager generic and makes adding a protocol (e.g.,
raw sockets later) a matter of providing callbacks, not modifying the socket layer.

---

## 4. Milestones

### Milestone 0: Foundation
**Goal:** Buildable skeleton + the foundation services from 3.4.

- [ ] cc370 + libc370 cross-compilation pipeline (MBT V2 project)
- [ ] Host build target for unit tests (Linux/macOS)
- [ ] Memory Manager (fixed pools, counters, exhaustion/leak detection)
- [ ] Buffer Manager (mbuf-like packet buffers, ownership model)
- [ ] Queue Library (generic queue primitives)
- [ ] Timer Manager (timer queue driven by STIMERM/TTIMER)
- [ ] Event Dispatcher + main loop skeleton (WAIT on ECB list)
- [ ] Trace Facility (per-subsystem flags)
- [ ] Statistics Manager (counter registry)
- [ ] PROFILE.TCPIP parser (minimal: DEVICE, LINK, HOME, GATEWAY, pool sizes, trace)
- [ ] STC skeleton: init → event loop → operator MODIFY (DISPLAY/TRACE/STATS/STOP) → clean shutdown
- [ ] Data structures defined (compact, documented, with size assertions)

**Deliverable:** A started task that initializes all foundation services, reads config,
runs the event loop, responds to operator commands, reports stats, and shuts down
cleanly. No packets yet — but the whole skeleton is alive and observable.

### Milestone 1: Device Driver — "Bits on the Wire"
**Goal:** Read and write raw IP packets via the CTCI adapter, behind the device
abstraction (3.6).

- [ ] Device abstraction interface (init/send/receive/shutdown)
- [ ] CTCI driver: EXCP/CCW chain for READ (assembler)
- [ ] CTCI driver: EXCP/CCW chain for WRITE (assembler)
- [ ] Async I/O handling (attention/completion interrupts, ECB/WAIT, I/O exit)
- [ ] Integration with buffer manager (receive into / send from pool buffers)
- [ ] PACKET_RECEIVED events flowing into the dispatcher
- [ ] Host loopback/pcap driver behind the same interface (for host testing)

**Deliverable:** The STC receives a raw IP packet from CTCI and dumps its hex to the
console via the trace facility; and sends a hand-crafted packet out.

### Milestone 2: IP + ICMP — "Ping!"
**Goal:** Process incoming IP, respond to ICMP echo. First real protocol on the
foundation.

- [ ] IP input: header validation, checksum verification, dispatch by protocol
- [ ] IP output: header construction, checksum calculation
- [ ] ICMP echo request/reply (ping responder)
- [ ] ICMP error messages (dest unreachable, time exceeded)
- [ ] IP routing (trivial default route via CTCI)
- [ ] Statistics + tracing wired into IP/ICMP

**Deliverable:** `ping <mvs-ip>` from the Hercules host works. The "it's alive!" moment.

### Milestone 3: UDP — "Simple Messages"
**Goal:** UDP send/receive, first socket API end-to-end (still in-process).

- [ ] UDP input/output + checksum
- [ ] UDP port demultiplexing to sockets
- [ ] Generic socket manager (3.14); UDP callbacks
- [ ] Socket API: SOCKET, BIND, SENDTO, RECVFROM, CLOSE (UDP)
- [ ] EZASOKET-compatible API surface (Phase 1: direct calls)

**Deliverable:** A UDP echo server on MVS; a host program sends a datagram and gets
the echo. First end-to-end socket API test.

### Milestone 4: TCP — "Connections"
**Goal:** Full TCP state machine.

- [ ] TCP state machine (all 11 states, RFC 793)
- [ ] Segment validation, sequence handling
- [ ] Three-way handshake; teardown (FIN/TIME-WAIT, via timer manager)
- [ ] Sliding window flow control (small window)
- [ ] Retransmission (timer manager; simple fixed RTO first)
- [ ] Socket API: CONNECT, LISTEN, ACCEPT, SEND, RECV, SELECT (TCP)
- [ ] Keepalive (configurable)

**Deliverable:** A TCP echo server on MVS; telnet from the host exchanges text with
proper handshake and teardown.

### Milestone 5: Subsystem Split + Hardening
**Goal:** Move to the Phase 2 deployment model and reach production-quality robustness.

- [ ] Subsystem STC: register via SSI (IEFSSNxx / dynamic), like mvslovers/ufsd
- [ ] IPC: SSREQ and/or SVC + cross-memory POST for completion
- [ ] EZASOKET library becomes a thin stub forwarding to the subsystem
  (applications unchanged)
- [ ] Cross-address-space serialization for shared request queue
- [ ] Proper TCP retransmission (exponential backoff, Karn's algorithm)
- [ ] Nagle (configurable), delayed ACK
- [ ] Congestion avoidance (simplified slow start / cwnd)
- [ ] RST handling/generation; out-of-order reassembly
- [ ] NETSTAT-like operator command (connections, statistics)
- [ ] Memory-pressure monitoring + graceful degradation
- [ ] Multi-connection stress testing
- [ ] SYS1.SAMPLIB install JCL, TSO HELP members, docs (operator + programmer guides)

### Milestone 6 (Stretch): Application Compatibility & LCS
**Goal:** Run existing applications on the native stack; add Ethernet.

- [ ] EZASOKET completeness vs. HTTPD requirements
- [ ] EZASOKET completeness vs. mvsMF requirements
- [ ] DNS resolver (UDP stub resolver)
- [ ] LCS device driver (Ethernet framing) behind the device abstraction
- [ ] ARP (required for LCS), with ARP-cache timers
- [ ] Multiple interface support + routing

---

## 5. Reference Material

### 5.1 RFCs (Core)
- **RFC 791** — IP · **RFC 792** — ICMP · **RFC 793** — TCP · **RFC 768** — UDP
- **RFC 826** — ARP (LCS milestone) · **RFC 1122** — Requirements for Internet Hosts

### 5.2 Books
- Douglas E. Comer, *Internetworking with TCP/IP Vol. 1* (concepts)
- Douglas E. Comer, *Internetworking with TCP/IP Vol. 2* (implementation/Xinu)
  - Xinu source: ftp://ftp.cs.purdue.edu/pub/comer/TCPIP-vol2.dist.tar.gz
  - Morrison's partial port: https://github.com/mvslovers/mvs38j-ip
  - **Reference only, not a code base**

### 5.3 IBM Documentation
- *z/OS Communications Server: IP Sockets API Guide and Reference* — EZASOKET spec
- *z/OS Communications Server: IP Configuration Reference* — PROFILE.TCPIP syntax
- *MVS/370 System Programming Library* — EXCP, SVC, CSA, SSI internals
- *IBM System/370 Principles of Operation* — instruction set

### 5.4 Existing Code (Reference)
- **mvslovers/mvs38j-ip** — Morrison's unfinished port (historical)
- **mvslovers/ufsd** — reference for the MVS subsystem model (Phase 2)
- **cc370 / libc370** — C compiler + target C runtime (built via MBT V2)
- **Shelby Beach's EZASMI** — existing EZASOKET impl on X'75' (API reference)
- **HTTPD (Mike Rayborn)** — primary compatibility test target
- **Jason Winter's x75.c / tcpip.c** — Hercules side of X'75'; shows which socket
  operations MVS programs actually use

### 5.5 Hercules Documentation
- CTCI configuration: https://sdl-hercules-390.github.io/html/hercinst.html
- CTC/CCW channel programming references

---

## 6. Project Conventions

### 6.1 Code Style
- **Comments and documentation:** English
- **C code:** consistent with libc370 conventions
- **Assembler:** structured macros where available; clear CSECT naming; register
  equates; libc370 linkage conventions
- **Module naming:** component prefixes (TBD — see Open Questions)
- **Header files:** one per component, minimal includes

### 6.2 Message Conventions
- IBM-style message IDs; prefix TBD (avoid colliding with real z/OS `EZA` IDs)
- Operator messages via WTO/WTOR; trace/debug gated by trace flags

### 6.3 Memory Discipline
- No unbounded allocations; every pool has a known maximum
- Fixed-size pools with configurable sizes; all allocation via the memory manager
- Every allocation accounted for (counters); document memory cost of each control
  block and pool in the source
- Structures designed compact with **field ordering to minimize padding** (prefer
  this over generic `packed`)

### 6.4 Testing Strategy
- Host build for isolated protocol/unit tests (see 3.10)
- On-system JCL test jobs per component
- Integration: ping, UDP echo, TCP echo from the Hercules host
- Ultimate test: HTTPD + mvsMF on the native stack

---

## 7. Risks & Open Questions

### 7.1 Known Risks
- **Memory:** 24-bit limit may cap connection count/throughput; profile from M0
- **cc370 quirks:** isolate suspect code generation via host-build unit tests and minimal repros
- **Phase 2 IPC complexity:** SSI + cross-memory + SVC is the hard MVS systems
  programming — deliberately deferred to M5, but non-trivial
- **CTCI timing:** known MIH complaints after long idle; handle in the driver
- **Scope creep:** full TCP is large; stick to milestones and the foundation-first order

### 7.2 Open Questions
- **Module/message prefix:** IBM-style `EZA*` (max compatibility, but looks like IBM
  code) vs. a project-specific prefix. Decide before M0.
- **Subsystem name (Phase 2):** 4-char SSI name (Morrison used `STCP`; `ufsd`
  provides a model). Decide before M5.
- **Max connections:** *configurable, bounded only by available memory* — no
  hard-coded number; document the per-connection cost so operators can size it.
- **Timer granularity:** is STIMER resolution sufficient for TCP RTO? Validate in M0.
- **Buffer size(s):** single fixed size vs. a few pool classes (small/large)?

---

## 8. Instructions for Claude Code

1. **Foundation first, protocols second.** Build the memory/buffer/queue/timer/event/
   trace/stats services (section 3.4) before IP/TCP — but only to a *minimum viable*
   state, then bring up ICMP on top to validate them. Don't gold-plate infrastructure
   in isolation.

2. **Everything is event-driven.** New work is a consumer of dispatcher events, not a
   new thread of control. Handlers run to completion.

3. **Strict interfaces between layers.** Protocol code never touches drivers directly
   (use the device abstraction) and never allocates raw storage (use the memory/buffer
   managers). This is what makes LCS and the Phase 2 subsystem split feasible later.

4. **Always consider the 24-bit memory constraint.** Before any data structure,
   compute its size and document it. Before any allocation, state where the memory
   comes from and its bound. Arrange fields to minimize padding.

5. **Buffer ownership, not copying.** Move buffer ownership through the stack; pass
   buffer objects between layers; send routines don't allocate.

6. **Write C first, optimize to assembler later** — but give hot-path C functions
   clean interfaces so an HLASM replacement needs no caller changes.

7. **Deployment is two-phase.** Phase 1 (M0–M4): in-process library, direct EZASOKET
   calls, one address space, easy debugging. Phase 2 (M5+): subsystem STC (like
   mvslovers/ufsd) with SSI/SVC/cross-memory; the EZASOKET library becomes a stub and
   applications stay unchanged. Don't build Phase 2 machinery during Phase 1.

8. **Follow IBM conventions** for the API (EZASOKET), configuration (PROFILE.TCPIP),
   operator interface, and message style.

9. **Statistics and tracing from day one.** Every component registers counters and
   honors its trace flag. It's nearly free and pays for itself in debugging.

10. **Use htons/ntohl wrappers** even though they're no-ops on S/370 — intent and
    host-test correctness.

11. **Keep protocol code host-compilable.** No MVS-specific dependencies in the
    protocol logic, so it can be unit-tested on Linux/macOS before running on Hercules.

12. **Reference the old mvs38j-ip and Xinu for ideas, never copy code.** Write fresh
    implementations from the RFCs and IBM docs.

13. **S/370 specifics:** 24-bit pointers (high byte may carry flags); Big Endian =
    network order (advantage, but still use the wrappers); EBCDIC (IP headers are
    binary; text data needs EBCDIC↔ASCII, ideally via TR); CS/CDS for
    interrupt-coordinated structures; EXCP for device I/O.

14. **Test incrementally.** Each milestone produces something demonstrable; don't move
    on until the current one works.

15. **This is an experiment.** Pragmatism over purity. If it works and fits in memory,
    it's good enough for now — refine later.

---

## Appendix A — Change Log

**v2 (this revision):** Incorporated external architecture review. Major changes:
- Reframed the whole project as an *event-driven network executive*; added the event
  dispatcher/main loop as the architectural core (3.1, 3.3).
- Expanded architecture with strict layer interfaces (3.2), a device abstraction
  layer (3.6), buffer ownership / mbuf-like model (3.5), generic socket layer (3.14),
  and a synchronization discipline (3.8).
- Promoted **Timer Manager, Trace Facility, Statistics** and a **Memory Manager** to
  first-class foundation components built in Milestone 0 (3.4, 3.7, 3.11, 3.12).
- **Replaced the CSA-first IPC plan** with a two-phase deployment model: Phase 1
  in-process library, Phase 2 subsystem STC (SSI, like mvslovers/ufsd) with the
  EZASOKET library degrading to a stub (3.9). This addresses the review's main
  criticism.
- Added host-based testing strategy (3.10) and byte-order wrapper guidance (3.13).
- Softened "packed structures" to "arrange fields to minimize padding"; changed
  max-connections from a fixed number to "configurable, memory-bounded."
- Reordered milestones so foundation services land in M0 and the subsystem split
  is an explicit M5 deliverable.
