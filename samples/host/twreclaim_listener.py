#!/usr/bin/env python3
"""twreclaim_listener.py -- host-side listener for the NSF M4-6 TIME_WAIT reclaim
live gate (test/mvs/tsttcpw.c).

Runs on the Hercules host (e.g. mvsdev) over the real CTCI link. The NSF guest is
the ACTIVE opener AND the ACTIVE closer: it connects, then closes first. For the
guest to end up in TIME_WAIT (not CLOSED), this listener must let the CLIENT close
first -- so it accepts, reads until EOF (the client's FIN), and only THEN closes
its own side. `nc -lk <port>` also works, but this is deterministic and counts.

Usage:
    python3 twreclaim_listener.py [host] [port] [--count N]

    host   bind address (default 192.168.200.2 -- the CTCI host peer)
    port   listen port    (default 3002 -- matches tsttcpw.c LISTEN_PORT)
    --count N  exit after N accepted connections (default: run forever)

Bring it up BEFORE starting the TSTTCPW job, with tcpdump watching:
    tcpdump -ni tun0 tcp port 3002 &
    python3 twreclaim_listener.py 192.168.200.2 3002 --count 40
Each cycle shows SYN / SYN,ACK / ACK then a clean 4-way FIN with the GUEST sending
the first FIN (the guest side goes TIME_WAIT). Exit code 0 once N connections were
served cleanly.
"""
import socket
import sys


def main():
    host = "192.168.200.2"
    port = 3002
    count = None

    args = sys.argv[1:]
    pos = []
    i = 0
    while i < len(args):
        if args[i] == "--count":
            count = int(args[i + 1])
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
    srv.listen(64)
    print("listening on %s:%d (count=%s)" % (host, port, count), flush=True)

    served = 0
    try:
        while count is None or served < count:
            conn, peer = srv.accept()
            # Read until the CLIENT closes (EOF) so the GUEST is the active closer
            # (-> guest TIME_WAIT). Discard any bytes; the guest sends none.
            try:
                while True:
                    data = conn.recv(4096)
                    if not data:
                        break
            finally:
                conn.close()          # our FIN only AFTER the client's FIN
            served += 1
            print("served %d from %s:%d" % (served, peer[0], peer[1]), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()

    print("done: served %d connection(s)" % served, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
