#!/usr/bin/env python3
"""echo_client.py -- host-side test client for the NSFECHO UDP echo server.

Runs on the Hercules host (e.g. mvsdev) against a live NSFECHO started with
jcl/NSFECHO.jcl. It exercises the NSF UDP path end to end over the real CTCI
link and prints a clear PASS/FAIL verdict per scenario, with a process exit
code (0 = PASS, non-zero = FAIL) so it drops into a CI/gate script.

Scenarios (subcommands):

  echo    N sequence-numbered datagrams, every reply verified byte-for-byte.
          The CTCI link is lossless (the M2 gate proved 1000/1000), so this
          asserts 100% replies -- UDP loss is NOT excused here.
  sizes   A payload sweep across the boundaries: 1 byte, a mid size, and the
          v1 maximum (MTU - 28). One oversize datagram (max + 1) must NOT be
          echoed -- it exceeds the v1 no-fragmentation limit and is dropped.
  kill9   Fork a child streaming echos, SIGKILL it mid-stream (no cleanup),
          then prove the server still answers a fresh burst. The server keeps
          no per-client state, so an abandoned client must leave nothing
          behind -- confirmed by the server's leak gate at shutdown.
  quit    Send the QUIT sentinel and assert the BYE reply arrives (this also
          terminates the server cleanly).
  gate    The composite M3 exit gate: echo -> sizes -> kill9 -> quit against
          one server run. PASS only if every step passes (and, separately,
          the server job then ends CC 0 -- check the joblog).

Stdlib only. Python 3.6+.

Examples:
  ./echo_client.py echo  --host 192.168.200.1 --port 7 --count 1000
  ./echo_client.py sizes --mtu 1500
  ./echo_client.py gate
"""

import argparse
import os
import signal
import socket
import sys
import time

# The QUIT sentinel and its BYE reply -- the exact ASCII bytes NSFECHO compares
# and sends. Raw bytes on the wire (the server never transcodes payload).
QUIT = b"QUIT"
BYE = b"BYE"

DEFAULT_HOST = "192.168.200.1"   # the NSF guest HOME address (CTCI .1)
DEFAULT_PORT = 7                 # the well-known echo port (NSFECHO default)
DEFAULT_TIMEOUT = 2.0            # per-datagram reply timeout, seconds
DEFAULT_COUNT = 1000
DEFAULT_MTU = 1500


def log(msg):
    print(msg, flush=True)


def verdict(name, ok, detail=""):
    """Print a uniform PASS/FAIL line and return ok."""
    tag = "PASS" if ok else "FAIL"
    line = "[{}] {}".format(tag, name)
    if detail:
        line += " -- " + detail
    log(line)
    return ok


def make_socket(host, port, timeout):
    """A connected UDP socket to the server (connect() also filters replies to
    the server's address, so a stray datagram cannot masquerade as an echo)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.connect((host, port))
    return s


def seq_payload(seq):
    """A distinct, self-identifying payload for datagram `seq`, so a stale or
    wrong reply is caught (not just a lost one)."""
    head = "NSFECHO seq={:08d} ".format(seq).encode("ascii")
    fill = bytes((seq + k) & 0xFF for k in range(16))
    return head + fill


def one_echo(sock, payload):
    """Send `payload`, return the reply bytes, or None if no reply arrives.

    A missing reply shows up two ways on a connected UDP socket: a timeout, or
    ConnectionRefusedError when the OS turned an ICMP port-unreachable (nobody
    listening / a dropped datagram) into an error on the next call. Both mean
    "not echoed" for our purposes."""
    try:
        sock.send(payload)
        return sock.recv(65535)
    except socket.timeout:
        return None
    except OSError:
        return None


# --------------------------------------------------------------------------- #
# Scenarios
# --------------------------------------------------------------------------- #

def scn_echo(args):
    """N datagrams, each reply verified byte-exact. Stop-and-wait keeps one
    datagram in flight so the (bounded) server queues never overrun and every
    reply is unambiguously matched to its request."""
    sock = make_socket(args.host, args.port, args.timeout)
    replied = 0
    mismatched = 0
    lost = 0
    first_bad = None
    t0 = time.time()
    try:
        for i in range(args.count):
            p = seq_payload(i)
            r = one_echo(sock, p)
            if r is None:
                lost += 1
                if first_bad is None:
                    first_bad = "seq {} timed out".format(i)
            elif r != p:
                mismatched += 1
                if first_bad is None:
                    first_bad = "seq {} reply mismatch".format(i)
            else:
                replied += 1
    finally:
        sock.close()
    dt = time.time() - t0
    ok = (replied == args.count) and (lost == 0) and (mismatched == 0)
    detail = "{}/{} echoed, {} lost, {} mismatched, {:.2f}s".format(
        replied, args.count, lost, mismatched, dt)
    if first_bad:
        detail += " (first: {})".format(first_bad)
    return verdict("echo", ok, detail)


def scn_sizes(args):
    """Boundary payload sweep + the oversize no-frag case."""
    maxpayload = args.mtu - 28          # IP(20) + UDP(8) headers
    cases = [
        ("1 byte", 1, True),
        ("mid ({}B)".format(maxpayload // 2), maxpayload // 2, True),
        ("max MTU-28 ({}B)".format(maxpayload), maxpayload, True),
        # max + 1 needs fragmentation, which v1 does not do (spec 11.2 / the
        # M3-3 no-frag policy): the server must NOT echo it -> expect a timeout.
        ("oversize ({}B)".format(maxpayload + 1), maxpayload + 1, False),
    ]
    sock = make_socket(args.host, args.port, args.timeout)
    all_ok = True
    try:
        for label, size, expect_echo in cases:
            payload = bytes((size + k) & 0xFF for k in range(size))
            reply = one_echo(sock, payload)
            if expect_echo:
                ok = (reply == payload)
                det = "echoed {}B".format(size) if ok else \
                    ("no/short reply" if reply is None else
                     "reply {}B != {}B".format(len(reply), size))
            else:
                ok = (reply is None)
                det = "correctly dropped (no fragmentation in v1)" if ok else \
                    "unexpectedly echoed {}B".format(len(reply))
            verdict("sizes: " + label, ok, det)
            all_ok = all_ok and ok
    finally:
        sock.close()
    return verdict("sizes", all_ok)


def _kill9_child(host, port):
    """Child process: stream echos as fast as replies come back, ignoring
    everything, until the parent SIGKILLs us. Never returns."""
    try:
        sock = make_socket(host, port, 1.0)
        i = 0
        while True:
            try:
                sock.send(b"child-stream-%d" % i)
                sock.recv(65535)
            except socket.timeout:
                pass
            i += 1
    except Exception:
        pass
    os._exit(0)


def scn_kill9(args):
    """Fork a streaming child, hard-kill it mid-stream, then prove the server
    still answers. The abandoned exchange must leave nothing behind server-side
    -- that assertion is the leak gate in the server's shutdown output."""
    pid = os.fork()
    if pid == 0:
        _kill9_child(args.host, args.port)
        os._exit(0)                     # unreachable
    # parent
    time.sleep(0.8)                     # let the child stream for a bit
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    log("kill9: child {} streamed, then SIGKILLed mid-stream".format(pid))
    time.sleep(0.3)                     # let any in-flight replies drain

    # A fresh burst: the server must still echo correctly.
    sock = make_socket(args.host, args.port, args.timeout)
    n = min(50, args.count)
    ok_count = 0
    try:
        for i in range(n):
            p = seq_payload(1_000_000 + i)
            if one_echo(sock, p) == p:
                ok_count += 1
    finally:
        sock.close()
    ok = (ok_count == n)
    return verdict("kill9", ok,
                   "server answered {}/{} after the hard kill".format(ok_count, n))


def scn_quit(args):
    """Send QUIT, assert BYE. This terminates the server."""
    sock = make_socket(args.host, args.port, args.timeout)
    try:
        reply = one_echo(sock, QUIT)
    finally:
        sock.close()
    ok = (reply == BYE)
    det = "got BYE" if ok else ("no reply" if reply is None else
                                "got {!r}".format(reply))
    return verdict("quit", ok, det)


def scn_gate(args):
    """The composite M3 exit gate. quit is LAST because it stops the server."""
    log("=== NSFECHO M3 exit gate: echo -> sizes -> kill9 -> quit ===")
    results = []
    results.append(("echo", scn_echo(args)))
    results.append(("sizes", scn_sizes(args)))
    results.append(("kill9", scn_kill9(args)))
    results.append(("quit", scn_quit(args)))
    all_ok = all(ok for _, ok in results)
    log("--- gate summary ---")
    for name, ok in results:
        log("  {:6s} {}".format(name, "PASS" if ok else "FAIL"))
    verdict("gate", all_ok,
            "the server job should now have ended CC 0 (check the joblog)")
    return all_ok


SCENARIOS = {
    "echo": scn_echo,
    "sizes": scn_sizes,
    "kill9": scn_kill9,
    "quit": scn_quit,
    "gate": scn_gate,
}


def main(argv):
    ap = argparse.ArgumentParser(
        description="Host-side test client for the NSFECHO UDP echo server.")
    ap.add_argument("scenario", choices=sorted(SCENARIOS),
                    help="which scenario to run")
    ap.add_argument("--host", default=DEFAULT_HOST,
                    help="server IP (default %(default)s, the NSF guest)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help="server UDP port (default %(default)s)")
    ap.add_argument("--count", type=int, default=DEFAULT_COUNT,
                    help="datagrams for echo (default %(default)s)")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                    help="per-datagram reply timeout, seconds (default %(default)s)")
    ap.add_argument("--mtu", type=int, default=DEFAULT_MTU,
                    help="link MTU for the sizes sweep (default %(default)s)")
    args = ap.parse_args(argv)

    log("NSFECHO client -> {}:{}  (timeout {}s)".format(
        args.host, args.port, args.timeout))
    ok = SCENARIOS[args.scenario](args)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
