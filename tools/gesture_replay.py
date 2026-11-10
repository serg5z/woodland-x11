#!/usr/bin/env python3
"""Replay a touch_diag.py --log session through the gesture recogniser (mt/x11_gesture.c).

    tools/gesture_replay.py build/touch/session-*.jsonl

The log holds what the display client saw at each poll (about 15 per second), so short
states can be missing; good enough to check that real two-finger motion is recognised.
Needs build/gesture_replay (make)."""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def lines(path):
    t0 = None
    for raw in open(path):
        r = json.loads(raw)
        t0 = r["t"] if t0 is None else t0
        down = [c for c in r["contacts"] if c["state"] != "UP"]
        yield f"{int((r['t'] - t0) * 1000) + 1} {len(down)} " + " ".join(f"{c['id']} {c['x']} {c['y']}" for c in down)


def main():
    feed = "\n".join(lines(sys.argv[1])) + "\n"
    r = subprocess.run([os.path.join(ROOT, "build", "gesture_replay")], input=feed, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
