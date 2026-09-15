#!/usr/bin/env python3
"""d1 §2.3 stimulus driver -- runs ON mvsdev, beside the emulator.

Makes A's listener read-ready by connecting to it and HOLDING the connection
open, and fires the arm-2 EDGE inside B's park window.

WHY IT WATCHES A CONSOLE MARKER RATHER THAN SLEEPING
    The connect must land AFTER B has allocated its own socket -- a passive
    child takes a socket-table slot (tcp_child_create -> soc_create), and a
    connect before B's socket would insert a child between A and B and break
    B's `own - 1` derivation, silently.  B therefore announces, and this script
    reacts to the announcement rather than guessing at a delay.

WHY IT HOLDS THE SOCKETS OPEN
    An RST dequeues the child from A's acceptq (tcp_do_reset -> end of life),
    which would clear the stimulus mid-round.  A FIN is tolerable (CLOSE_WAIT,
    still queued, still read-ready) but holding is the discipline.  Do not kill
    this script -- let it reach its deadline, which closes cleanly.

CLOCKS
    Every line is stamped with THIS HOST's clock, the same clock tcpdump uses.
    The MVS console runs UTC-5; no cross-clock arithmetic belongs in a
    load-bearing step, so the park window is bracketed here, in host time.

THE WATCHER VALIDATES ITSELF FIRST
    It waits for A's `A HOLDING SOCKET` line before arming for B's marker.  A
    watcher that silently matches nothing is indistinguishable from a round in
    which the marker never appeared (CLAUDE.md 8.5), so the positive control is
    built in: no HOLDING line, no run.

Usage:  d1stim.py [--log PATH] [--peer IP] [--port N] [--deadline SEC]
"""
import argparse, socket, sys, time, os

def ts():
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()) + \
           ".%03d" % int((time.time() % 1) * 1000)

def say(msg):
    print("%s  %s" % (ts(), msg), flush=True)

def connect_hold(peer, port, held, tag):
    try:
        s = socket.create_connection((peer, port), timeout=10)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x00" * 8)
        held.append(s)
        say("CONNECT %s -> %s:%d OK  (local port %d, HELD OPEN)"
            % (tag, peer, port, s.getsockname()[1]))
        return True
    except Exception as exc:                      # noqa: BLE001
        say("CONNECT %s -> %s:%d FAILED: %s" % (tag, peer, port, exc))
        return False

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=os.path.expanduser("~/MVSCE-DEV/mvslog.txt"))
    ap.add_argument("--peer", default="192.168.200.1")
    ap.add_argument("--port", type=int, default=3011)
    ap.add_argument("--deadline", type=int, default=420)
    a = ap.parse_args()

    held = []
    t_end = time.time() + a.deadline
    say("START  log=%s peer=%s:%d deadline=%ds" % (a.log, a.peer, a.port, a.deadline))

    # Seek to EOF so a marker from a PREVIOUS run can never trigger this one.
    f = open(a.log, "r", errors="replace")
    f.seek(0, os.SEEK_END)
    say("watching from byte %d (end of file) -- earlier runs cannot trigger us"
        % f.tell())

    seen_holding = False
    fired = False
    while time.time() < t_end:
        line = f.readline()
        if not line:
            time.sleep(0.2)
            continue
        if "TSTD1B: A HOLDING SOCKET" in line:
            seen_holding = True
            say("SAW A HOLDING  (watcher POSITIVE CONTROL: it can match) | %s"
                % line.strip()[:110])
        elif "TSTD1B: B SWEEP DONE" in line:
            if not seen_holding:
                say("REFUSING TO FIRE: B's marker arrived but A's HOLDING line"
                    " never did -- the watcher is not validated, so a connect"
                    " now would be unattributable.")
                break
            say("TRIGGER 1 (B swept, A-desc announced) | %s" % line.strip()[:110])
            connect_hold(a.peer, a.port, held, "#1/pre-arm")
        elif "TSTD1B: PARKING SELECT ON" in line:
            say("TRIGGER 2 (B is parking) -- PARK WINDOW OPENS NOW | %s"
                % line.strip()[:110])
            fired = True
            # The EDGE: fresh graduations while B is parked.  Each one fires
            # soc_notify_ready on A's listener unconditionally, which re-scans
            # every parked SELECT -- B's included.
            connect_hold(a.peer, a.port, held, "#2/EDGE t+0")
            time.sleep(3.0)
            connect_hold(a.peer, a.port, held, "#3/EDGE t+3")
            say("park-window connects done; holding everything open")
        elif "TSTD1B: PARKED SELECT RC=" in line:
            say("PARK WINDOW CLOSES | %s" % line.strip()[:110])
        elif "TSTD1B: A NEVER READY" in line:
            say("A REPORTS NO STIMULUS -- the round will skip | %s"
                % line.strip()[:110])
        elif "TSTD1B: B DONE" in line:
            say("B DONE -- holding open a further 30 s for A's final polls")
            t_end = min(t_end, time.time() + 30)

    say("deadline reached; fired_park_connects=%s held=%d" % (fired, len(held)))
    for s in held:
        try:
            s.close()
        except Exception:                          # noqa: BLE001
            pass
    say("END (sockets closed cleanly -- FIN, never RST)")
    return 0 if fired else 1

if __name__ == "__main__":
    sys.exit(main())
