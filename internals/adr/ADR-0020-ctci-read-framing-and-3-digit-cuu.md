# ADR-0020 — CTCI READ framing (one block, no 0x0000 terminator to the guest) and the 3-hex-digit SVC 99 unit name

**Status:** Accepted (2026-07-12), from the first live EXCP run of the CTCI
driver on real MVS (issue #16). Corrects two things in §9.3 that were verified
against docs/older code but not against a running 3088 CTCI device.
**Relates to:** §9.3 (CTCI driver), ADR-0019 (EXCP + IOB ECB), issue #16,
CLAUDE.md §7 (M1-3), [[nsf370-device-number-width]].

## Context

M1-3 built the CTCI top half + C lifecycle and proved the SVC 99 seam against
undefined / DUMMY units, but the **EXCP READ/WRITE channel path had never run**
— there was no Hercules CTCI device. A fresh MVSCE on Hercules with a real
`0500,0501 CTCI 192.168.200.1 192.168.200.2` pair on `tun0` cleared that block,
and the first live run surfaced two defects the paper design hid. Both are the
kind of "looked-proven-but-wasn't" the issue explicitly warned about.

### 1. SVC 99 `DALUNIT` unit name is 3 hex digits, not 4

`ctci_dev_open` formatted the unit name with `snprintf("%04X", cuu)` → `"0500"`.
The allocate failed `S99ERROR 021C` ("the unit name specified is undefined") —
**even though device 0500 was defined and online.** MVS 3.8j (OS/VS2 R3.8,
S/370) device numbers are **3 hex digits** (CUU); the 4-digit device number
arrived with S/370-XA. A 4-digit unit name matches no device, generic, or
esoteric group, so `021C`. `__txunit` sends the C string verbatim at `strlen`,
so `"0500"` really is a 4-char (wrong) name.

This also invalidated an earlier "proof": the M1-3 wall-probe that allocated the
*undefined* CUU `0E20` and "passed" (expecting failure) was a **false positive**
— a 4-digit name is undefined for **any** value, so `021C` proved nothing about
`__txunit` succeeding.

### 2. §9.3 READ framing was wrong (the guest reads one block, no 0x0000 terminator)

§9.3 (from M1-1, "byte-exact verified") modelled the device buffer as a *chain
of blocks* walked via `hwOffset` to a terminating `hwOffset = 0x0000` block. The
live READ delivered a block whose leading `hwOffset` equalled the transferred
length, with no reachable `0x0000` — the test's chain-walk failed. Reading the
**exact Hercules running on the host** (`ctc_ctci.c` `CTCI_Read` /
`CTCI_EnqueueIPFrame`) resolved it:

- `CTCI_EnqueueIPFrame` uses **one** block header (`pFrame = bFrameBuffer`) and
  appends each frame as a **segment**; it bumps that single header's `hwOffset`
  to `iFrameOffset + sizeof(CTCIHDR)` = end-of-segments. So it is **one block of
  many segments**, not a chain of blocks.
- `hwType` is hard-coded `ETH_TYPE_IP` (**0x0800**) for **every** frame — the
  live READ carried an IPv6 MLD packet stamped `0x0800`. It is a constant marker,
  not a v4/v6 discriminator.
- `CTCI_Read` stores a terminating `hwOffset = 0x0000` into Hercules' *own*
  buffer but **does not transfer it to the guest**: the Vince Weaver "day-1 bug"
  fix sets `iLength = iFrameOffset + sizeof(CTCIHDR)` (dropping the `+2`), so the
  guest's data ends at `hwOffset` and there is **no `0x0000` halfword** to walk to.

The **WRITE** framing was correct: the guest builds `[CTCIHDR hwOffset=end]
[CTCISEG][IP][hwOffset=0x0000]` and Hercules reads to the zero — a crafted ICMP
echo framed that way wrote post `X'7F'` and reached the host TUN unchanged.

## Decision

1. Format the SVC 99 unit name as **3 hex digits** — `snprintf("%03X", cuu)`.
2. Correct §9.3's READ framing: **one block of `CTCISEG`s**, leading `hwOffset` =
   end-of-data, **no `0x0000` terminator transferred to the guest**; the reader
   walks segments by `hwLength` from `sizeof(CTCIHDR)` up to `hwOffset`. `hwType`
   is a constant `0x0800` marker; the IP version comes from the packet. The WRITE
   side (append a terminating `0x0000` block) is unchanged and validated.
3. The production `CTCISEG`↔PBUF codec, PBUF conversion, DEVOPS, READ
   re-drive/ping-pong and MIH handling remain **M1-4**.

## Consequences

- M1-3's EXCP channel path is validated (TSTCTCM CC 0, 12/12, both legs) and its
  deferred-seam labels are removed.
- The M1-4 codec must decode by the corrected model (walk `hwLength` to
  `hwOffset`; never look for `0x0000`; read the IP version from the packet, drop
  non-IPv4 since v1 is IPv4-only).
- `MAX_CTCI_FRAME_SIZE` and buffer sizing are unaffected.

## Evidence (live, issue #16, MVSCE on `mvsdev`)

- `IEF237I 500/501 ALLOCATED`, `NSF210I CTCI 0500/0501 UP DD SYS00005/SYS00007`
  — two distinct DDNAMEs; SVC 99 `__txunit` success against a defined CUU.
- `WRITE post=7F len=38` + `tcpdump`: `192.168.200.1 > 192.168.200.2: ICMP echo
  request, id 43981 (0xABCD), seq 1` — the crafted block on the wire.
- `READ post=7F len=20432` (residual arithmetic), block walked to
  `227 well-formed CTCISEG(s)` by `hwLength`.
- Primary source: `ctc_ctci.c` `CTCI_Read` / `CTCI_EnqueueIPFrame`, read on the
  running Hercules host.
