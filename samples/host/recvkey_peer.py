#!/usr/bin/env python3
"""recvkey_peer.py -- host-side peer for the 80-CHK cross-AS receive key probe
(test/mvs/tstrqxr.c, issue #80).

Runs on the Hercules host (e.g. mvsdev) over the real CTCI link. It is a UDP
peer, deliberately trivial: it answers two triggers from the NSF guest.

    trigger "R0"  ->  reply with a ZERO-LENGTH datagram   (the CONTROL)
    trigger "R1"  ->  reply with --len pattern bytes      (the ARM)

Why a zero-length reply is the control: on the guest side both replies are
delivered by the SAME function (udp_complete_recv), which calls

    buf_copyout(bpay, r->ubuf, want)      want = min(paylen, r->ulen)

With paylen == 0 the call is still made but buf_copyout's loop never runs, so
the memcpy into the key-0 CSA staging buffer never happens. The two replies
therefore differ by the STORE and by nothing else.

DELAY: each reply is held back by --delay seconds so the guest is provably
PARKED on its recvfrom when the datagram arrives -- that drives the parked
completion path (device -> nsfudp_input -> udp_complete_recv) with no
guest-side timing.

EXPECT THE ARM TO KILL THE GUEST'S STACK. If the prediction holds, NSFS abends
S0C4 on the second reply and this peer simply never hears from the guest again.
That is a successful run, not a failure, so the exit code below reports what
actually happened rather than pass/fail.

TCP MODE (--tcp, 80-FIX Stage A / Stage C): the same probe one transport over.
The guest is the ACTIVE opener; this peer accepts ONE connection, waits --delay,
and sends --len pattern bytes, so the guest's nsf_recv is a data-returning
receive that reaches the identical store through the TCP path
(nsftcp.c -> buf_copyout -> memcpy) instead of the UDP one. TCP is what HTTPD
and mvsMF would use at M6, which is why it is measured rather than reasoned.

Usage:
    python3 recvkey_peer.py [host] [port] [--len N] [--delay S] [--tcp]

    host    bind address (default 192.168.200.2 -- the CTCI host peer)
    port    listen port  (default 3004 -- matches tstrqxr.c PEER_PORT)
    --len   payload bytes in the second reply (default 256)
    --delay seconds to wait before each reply (default 3)
    --tcp   TCP mode: accept one connection and send --len bytes
"""
import socket
import sys
import time


def pat(i):
    return (i * 7 + (i >> 5) + 0x23) & 0xFF


def tcp_mode(host, port, payload, delay):
    """Accept ONE connection and send `payload` after `delay`.

    The delay is the same discipline as the UDP path: it puts the guest
    provably inside its recv when the data arrives, so the parked completion
    path runs with no guest-side timing.
    """
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    srv.settimeout(300)
    print("TCP peer on %s:%d -- will send %d bytes after %.1fs"
          % (host, port, len(payload), delay), flush=True)

    try:
        conn, who = srv.accept()
    except socket.timeout:
        print("TIMEOUT waiting for the guest to connect", flush=True)
        srv.close()
        return 2
    print("connected from %s:%d" % (who[0], who[1]), flush=True)

    time.sleep(delay)                   # guest is parked in recv by now
    conn.sendall(payload)
    print("  sent %d-byte payload (the arm)" % len(payload), flush=True)

    # Hold the connection open briefly so a surviving guest can read it all
    # before we FIN; a dead guest simply never reads.
    time.sleep(delay)
    conn.close()
    srv.close()
    print("RESULT: payload sent -- read the MVS console for the verdict",
          flush=True)
    return 0


def main():
    host = "192.168.200.2"
    port = 3004
    length = 256
    delay = 3.0
    tcp = False

    args = sys.argv[1:]
    pos = []
    i = 0
    while i < len(args):
        if args[i] == "--len":
            length = int(args[i + 1])
            i += 2
        elif args[i] == "--delay":
            delay = float(args[i + 1])
            i += 2
        elif args[i] == "--tcp":
            tcp = True
            i += 1
        else:
            pos.append(args[i])
            i += 1
    if len(pos) >= 1:
        host = pos[0]
    if len(pos) >= 2:
        port = int(pos[1])

    payload = bytes(pat(i) for i in range(length))

    if tcp:
        return tcp_mode(host, port, payload, delay)

    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.settimeout(300)
    print("peer on %s:%d -- reply 1 = 0 bytes, reply 2 = %d bytes, delay %.1fs"
          % (host, port, length, delay), flush=True)

    replies = 0
    while replies < 2:
        try:
            data, who = srv.recvfrom(2048)
        except socket.timeout:
            print("TIMEOUT waiting for trigger %d" % (replies + 1), flush=True)
            break
        print("trigger %d from %s:%d -> %r" % (replies + 1, who[0], who[1],
                                               data), flush=True)
        time.sleep(delay)               # guest is parked in recvfrom by now
        if replies == 0:
            srv.sendto(b"", who)
            print("  sent ZERO-LENGTH reply (the control)", flush=True)
        else:
            srv.sendto(payload, who)
            print("  sent %d-byte reply (the arm)" % length, flush=True)
        replies += 1

    if replies < 2:
        print("RESULT: only %d of 2 triggers arrived" % replies, flush=True)
        srv.close()
        return 2

    # If the guest's stack survived the arm it will normally go quiet anyway
    # (the test closes and ends), so silence here proves nothing either way.
    print("RESULT: both replies sent -- read the MVS console for the verdict",
          flush=True)
    srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
