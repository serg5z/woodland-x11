#!/usr/bin/env python3
"""Touch calibration check for the X11 experiment (Milestone 3A).

Shows five targets one at a time on the whole screen (corners inset 40 px, then the
centre), records where each tap arrives as an X ButtonPress, and prints the error. If the
error is noticeable, it prints the TOUCHCAL line for the panel's serial console that maps
the measured raw range onto the screen. Measure with no calibration set (TOUCHCAL RESET).

    python3 tools/touch_targets.py --display PANEL_IP:0      (cookie from `xauth list`)
    python3 tools/touch_targets.py --display 127.0.0.1:99 --no-auth   (host server tests)

The cookie is read from xauth into memory and sent only in the connection setup; it is
never printed or written anywhere."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tests"))
from x11_raw import ROOT, Raw, decode_event  # noqa: E402

W, H = 800, 480
TARGETS = [(40, 40), (759, 40), (40, 439), (759, 439), (400, 240)]
GOOD_PX = 6  # errors up to this need no calibration


def cookie_for(display: str) -> bytes:
    out = subprocess.run(["xauth", "list", display], capture_output=True, text=True, check=False).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] == "MIT-MAGIC-COOKIE-1":
            return bytes.fromhex(parts[2])
    raise SystemExit(f"no MIT-MAGIC-COOKIE-1 for {display} in xauth (add it with xauth add)")


def fit(targets: list[int], measured: list[int], size: int) -> tuple[int, int]:
    """Least squares measured = a * target + b; returns the raw values at 0 and size-1."""
    n = len(targets)
    mt, mm = sum(targets) / n, sum(measured) / n
    var = sum((t - mt) ** 2 for t in targets)
    a = sum((t - mt) * (m - mm) for t, m in zip(targets, measured)) / var
    b = mm - a * mt
    return round(b), round(a * (size - 1) + b)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--display", required=True, help="host:N, as in DISPLAY")
    ap.add_argument("--no-auth", action="store_true", help="no cookie (the host test server only)")
    ap.add_argument("--timeout", type=float, default=60, help="seconds to wait for each tap")
    a = ap.parse_args()
    host, _, num = a.display.rpartition(":")
    num = num.split(".")[0]
    auth = (b"", b"") if a.no_auth else (b"MIT-MAGIC-COOKIE-1", cookie_for(a.display))
    r = Raw(6000 + int(num), host=host or "127.0.0.1", auth_name=auth[0], auth_data=auth[1])
    win = r.create_window(ROOT, 0, 0, W, H, events=0x4 | 0x8, bg=0x0000, override=True)  # press, release
    white = r.create_gc(win, 0xFFFF)
    r.map(win)
    r.sync()
    measured = []
    for i, (tx, ty) in enumerate(TARGETS):
        r.clear(win)
        r.fill_rects(win, white, [(tx - 30, ty, 61, 1), (tx, ty - 30, 1, 61), (tx - 4, ty - 4, 9, 9)])
        r.sync()
        print(f"target {i + 1}/{len(TARGETS)}: tap the cross at ({tx}, {ty})", flush=True)
        deadline = time.time() + a.timeout
        press, released = None, False
        while press is None or not released:
            if time.time() > deadline:
                print("timed out waiting for a tap")
                return 2
            for m in r.poll_events(0.2):
                e = decode_event(m)
                if e["code"] == 4 and press is None:
                    press, released = e, False
                elif e["code"] == 5 and press is not None:
                    released = True
        mx, my = press["root_x"], press["root_y"]
        measured.append((mx, my))
        print(f"  touch at ({mx}, {my}), error {mx - tx:+d}, {my - ty:+d}", flush=True)
    errs = [max(abs(mx - tx), abs(my - ty)) for (mx, my), (tx, ty) in zip(measured, TARGETS)]
    print(f"largest error {max(errs)} px, mean {sum(errs) / len(errs):.1f} px")
    if max(errs) <= GOOD_PX:
        print(f"calibration not needed (all within {GOOD_PX} px)")
    else:
        x0, x1 = fit([t[0] for t in TARGETS], [m[0] for m in measured], W)
        y0, y1 = fit([t[1] for t in TARGETS], [m[1] for m in measured], H)
        print("suggested (panel serial console, measured without calibration):")
        print(f"TOUCHCAL {x0} {x1} {y0} {y1}")
    r.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
