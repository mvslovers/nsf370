#!/usr/bin/env python3
"""M5-2d1 live gate 2.1 -- the host peer for the cross-AS ACCEPT.

Connects to the guest's listener, sends one line, and requires it back byte for
byte. The echo is what proves a verb on the ACCEPTED CHILD was permitted: the
child is created by the stack (tcp_child_create) and inherits the listener's
apptok, so if d1's ownership check is one degree too strict the guest refuses
it and no echo comes back.

  python3 d1accept_peer.py 192.168.200.1 3009
"""
import socket
import sys


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.200.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 3009
    payload = b"D1-ACCEPT-GATE\n"

    print(f"connecting to {host}:{port} ...", flush=True)
    with socket.create_connection((host, port), timeout=60) as s:
        s.settimeout(60)
        s.sendall(payload)
        print(f"sent {len(payload)} bytes", flush=True)
        got = b""
        while len(got) < len(payload):
            chunk = s.recv(len(payload) - len(got))
            if not chunk:
                break
            got += chunk

    if got == payload:
        print(f"PASS: echoed {len(got)} bytes, byte-exact")
        return 0
    print(f"FAIL: sent {payload!r}, got {got!r}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
