#!/usr/bin/env python3
"""shortwrite_listener.py -- host-side peer for the NSF M5-2a cross-AS short-write
live gate (test/mvs/tstrqxm.c).

Runs on the Hercules host (e.g. mvsdev) over the real CTCI link. The NSF guest is
the ACTIVE opener: it connects, then streams a deterministic pattern using
SHORT WRITES -- each nsf_send returns at most NSFREQX_CHUNK (2048) bytes, because
that is what one CSA staging chunk carries across the address-space boundary, and
the guest loops on the returned count exactly as BSD send semantics require.

This listener accepts ONE connection, reads until EOF, and verifies every byte
against the same generator the guest uses:

    pat(i) = (i * 7 + (i >> 5) + 0x23) & 0xFF

A byte-exact result proves the data really crossed two boundaries -- the guest
application's address space into the NSFS STC's, and the STC onto the wire --
with the moved counts the guest was told.

Usage:
    python3 shortwrite_listener.py [host] [port] [--expect N]

    host      bind address (default 192.168.200.2 -- the CTCI host peer)
    port      listen port  (default 3003 -- matches tstrqxm.c SW_PORT)
    --expect  exact byte count required (default 9353 -- see tstrqxm.c)

Bring it up BEFORE submitting the TSTRQXM job:
    python3 shortwrite_listener.py 192.168.200.2 3003 --expect 9353

Exit 0 when exactly --expect bytes arrived and every one matched; 1 otherwise.
"""
import socket
import sys


def pat(i):
    return (i * 7 + (i >> 5) + 0x23) & 0xFF


def main():
    host = "192.168.200.2"
    port = 3003
    expect = 9353

    args = sys.argv[1:]
    pos = []
    i = 0
    while i < len(args):
        if args[i] == "--expect":
            expect = int(args[i + 1])
            i += 2
        else:
            pos.append(args[i])
            i += 1
    if len(pos) >= 1:
        host = pos[0]
    if len(pos) >= 2:
        port = int(pos[1])

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    srv.settimeout(300)
    print("listening on %s:%d, expecting %d bytes" % (host, port, expect),
          flush=True)

    try:
        conn, peer = srv.accept()
    except socket.timeout:
        print("FAIL: no connection within 300 s", flush=True)
        return 1
    print("connected from %s:%d" % peer, flush=True)

    conn.settimeout(120)
    data = bytearray()
    while True:
        try:
            chunk = conn.recv(65536)
        except socket.timeout:
            print("FAIL: read timed out after %d bytes" % len(data), flush=True)
            break
        if not chunk:
            break                       # guest FIN
        data.extend(chunk)
    conn.close()
    srv.close()

    print("received %d bytes" % len(data), flush=True)

    bad = -1
    for i in range(min(len(data), expect)):
        if data[i] != pat(i):
            bad = i
            break

    if bad >= 0:
        print("FAIL: first mismatch at offset %d: got %02X want %02X"
              % (bad, data[bad], pat(bad)), flush=True)
        return 1
    if len(data) != expect:
        print("FAIL: expected %d bytes, got %d" % (expect, len(data)),
              flush=True)
        return 1

    print("PASS: %d bytes byte-exact against the pattern" % expect, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
