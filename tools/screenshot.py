#!/usr/bin/env python3
"""Screenshot of a Woodland X11 screen over the network, saved as a PNG on this computer.

A plain X11 client (no libraries): it connects to the server like any application (Wi-Fi for
the panel, TCP for the host server), reads the root window with GetImage (ZPixmap, depth 16,
RGB565) and writes an 8-bit RGB PNG. Nothing on the server changes; the cookie comes from
`xauth list DISPLAY` (as for any X client), or --no-auth for a host server started without one.

    tools/screenshot.py --display PANEL_IP:0 --out screen.png
    tools/screenshot.py --display 127.0.0.1:1 --no-auth --out screen.png
"""
import argparse
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from png_tools import write_png  # noqa: E402
from touch_diag import X, cookie_for, req  # noqa: E402

GET_IMAGE, Z_PIXMAP = 73, 2


def expand(v: int) -> tuple:
    """RGB565 -> 8-bit RGB, the same expansion as the host server's --png (bit replication)."""
    r5, g6, b5 = v >> 11, (v >> 5) & 63, v & 31
    return (r5 << 3 | r5 >> 2, g6 << 2 | g6 >> 4, b5 << 3 | b5 >> 2)


def grab(x: X, band: int = 96) -> list:
    """The root window as rows of (r, g, b), read in bands of `band` rows."""
    rows = []
    for y0 in range(0, x.height, band):
        h = min(band, x.height - y0)
        r = x.reply(x.send(req(GET_IMAGE, Z_PIXMAP,
                               struct.pack("<IhhHHI", x.root, 0, y0, x.width, h, 0xFFFFFFFF))))
        if r[1] != 16:
            raise SystemExit(f"unexpected root depth {r[1]} (this tool reads RGB565 servers)")
        data = r[32:]
        stride = (x.width * 2 + 3) & ~3          # ZPixmap rows are padded to 32 bits
        for row in range(h):
            line = data[row * stride:row * stride + x.width * 2]
            rows.append([expand(v) for (v,) in struct.iter_unpack("<H", line)])
    return rows


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--display", required=True, help="host:N, as in DISPLAY")
    ap.add_argument("--out", required=True, help="PNG file to write")
    ap.add_argument("--no-auth", action="store_true", help="connect without a cookie (host test server)")
    ap.add_argument("--band", type=int, default=96, help="rows per GetImage request (default 96)")
    a = ap.parse_args(argv)
    host, _, num = a.display.rpartition(":")
    t0 = time.monotonic()
    x = X(host or "127.0.0.1", 6000 + int(num.split(".")[0]), None if a.no_auth else cookie_for(a.display))
    rows = grab(x, max(1, a.band))
    x.s.close()
    write_png(a.out, rows)
    print(f"{a.out}: {x.width}x{x.height}, {time.monotonic() - t0:.2f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
