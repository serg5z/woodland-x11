#!/usr/bin/env python3
"""Touch-pipeline probe: simulated GT9xx frames (15 ms apart, as the panel polls) through the
host server's multi-touch chain (mt/x11_touchchain + the single-touch filter), with an X
client counting the button events it receives. Each case prints the X events and the gate and
recogniser counters, to find accidental Button1 clicks when the number of contacts changes.

    touch_probe.py BUILD/x11_host_server"""
import os
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from input_diff import Client, M  # noqa: E402

SERVER = sys.argv[1]


class Run:
    def __init__(self, display):
        self.p = subprocess.Popen([SERVER, "-d", str(display), "--input", "-q"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        time.sleep(0.4)
        self.c = Client(6000 + display, "probe")
        self.c.select(self.c.root, M["press"] | M["release"])
        self.c.sync()
        self.t = 1000

    def frame(self, *pts):
        self.t += 15
        line = f"mt {self.t} {len(pts)} " + " ".join(f"{i} {x} {y}" for i, x, y in pts)
        self.p.stdin.write(line.strip() + "\n")
        self.p.stdin.flush()
        self.p.stdout.readline()

    def idle(self, n=8):
        for _ in range(n):
            self.frame()

    def events(self):
        self.c.sync()
        time.sleep(0.05)
        self.c.pump()
        out = [("P" if e[0] & 0x7F == 4 else "R") + str(e[1]) for e in self.c.events if e[0] & 0x7F in (4, 5)]
        self.c.events = []
        return out

    def diag(self, keys):
        r = self.c.roundtrip(16, 0, struct.pack("<HH", 14, 0) + b"_WOODLAND_DIAG")
        atom = struct.unpack_from("<I", r, 8)[0]
        r = self.c.roundtrip(20, 0, struct.pack("<IIIII", self.c.root, atom, 0, 0, 4096))
        text = r[32:32 + struct.unpack_from("<I", r, 16)[0]].decode(errors="replace")
        return {k: (re.search(r"^%s=(\S+)" % re.escape(k), text, re.M) or [None, "?"])[1] for k in keys}

    def close(self):
        self.c.s.close()
        self.p.stdin.close()
        self.p.terminate()
        self.p.wait(5)


KEYS = ["gate.taps", "gate.commits", "gate.discards", "gate.blocked_active", "g.taps", "g.cancels", "g.scroll_v",
        "g.x11_button3", "g.x11_button4", "g.x11_button5", "g.x11_deferred"]


def case(name, fn, display):
    r = Run(display)
    fn(r)
    r.idle(10)
    ev = r.events()
    d = r.diag(KEYS)
    r.close()
    print(f"{name:52s} X: {' '.join(ev) or '-':28s} " + " ".join(f"{k.split('.')[1]}={v}" for k, v in d.items() if v not in ("0", "?")))


def two_finger_tap(delay_frames):
    def f(r):
        r.frame((1, 300, 200))
        for _ in range(delay_frames - 1):
            r.frame((1, 300, 200))
        for _ in range(3):
            r.frame((1, 300, 200), (2, 400, 200))
    return f


def two_finger_scroll(delay_frames):
    def f(r):
        for _ in range(delay_frames):
            r.frame((1, 300, 200))
        for i in range(12):
            r.frame((1, 300, 200 + 10 * i), (2, 400, 200 + 10 * i))
    return f


def lift_one_of_two(r):
    for _ in range(3):
        r.frame((1, 300, 200), (2, 400, 200))
    for i in range(10):  # the second finger lifts, the first keeps moving
        r.frame((1, 300 + 5 * i, 200))


def drag_then_second(r):
    for i in range(8):
        r.frame((1, 300 + 5 * i, 200))  # an active drag (Button1 down)
    for _ in range(4):
        r.frame((1, 340, 200), (2, 450, 200))
    for i in range(5):
        r.frame((1, 340 + 5 * i, 200))


def double_tap(gap_frames):
    def f(r):
        for _ in range(2):
            r.frame((1, 300, 200))
        for _ in range(gap_frames):
            r.frame()
        for _ in range(2):
            r.frame((1, 302, 201))
    return f


def three_lift_one_by_one(r):  # input audit B5
    for _ in range(2):
        r.frame((1, 100, 200), (2, 300, 200), (3, 500, 200))
    r.frame((1, 100, 200), (2, 300, 200))
    r.frame((1, 100, 200))


def five_staggered(r):
    for k in range(1, 6):
        for _ in range(2):
            r.frame(*[(i + 1, 100 + 100 * i, 200) for i in range(k)])
    for k in range(5, 0, -1):
        r.frame(*[(i + 1, 100 + 100 * i, 200) for i in range(k)])


if __name__ == "__main__":
    d = 640
    for n in (1, 2, 3, 4, 5, 6, 10):
        d += 1
        case(f"two-finger tap, second finger after {15 * n} ms", two_finger_tap(n), d)
    for n in (1, 3, 4, 6):
        d += 1
        case(f"two-finger scroll, second finger after {15 * n} ms", two_finger_scroll(n), d)
    d += 1
    case("two fingers, one lifts, the other moves on", lift_one_of_two, d)
    d += 1
    case("one-finger drag, then a second finger", drag_then_second, d)
    for g in (2, 3, 4, 6):
        d += 1
        case(f"double tap, {15 * g} ms between the taps", double_tap(g), d)
    d += 1
    case("three together, lift 3->2->1", three_lift_one_by_one, d)
    d += 1
    case("1..5 fingers landing 30 ms apart, lifting", five_staggered, d)
