#!/usr/bin/env python3
"""Differential GetImage check: the same pixmaps (depth 16 and 1, odd widths, a pattern of
pixel values) read back with ZPixmap and XYPixmap and several plane masks, on a reference
server (Xvfb at depth 16) and on the Woodland host server. Compares the reply depth, length
and image bytes (padding included) exactly; the visual is 0 for pixmaps on both.

    getimage_diff.py REF_PORT TEST_PORT"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from cmap_diff import default_colormap  # noqa: E402,F401
from font_diff import X  # noqa: E402

ZPIXMAP, XYPIXMAP = 2, 1


def root_of(port):
    import socket
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(b"l\0\x0b\0\0\0\0\0\0\0\0\0")
    h = s.recv(8)
    n = struct.unpack("<H", h[6:8])[0] * 4
    d = b""
    while len(d) < n:
        d += s.recv(n - len(d))
    o = 32 + ((struct.unpack_from("<H", d, 16)[0] + 3) & ~3) + 8 * d[21]
    s.close()
    return struct.unpack_from("<I", d, o)[0]


def run(port):
    x = X(port)
    root = root_of(port)
    out = []
    for depth, w, h in ((16, 7, 3), (16, 1, 2), (1, 9, 2), (1, 33, 1)):
        pm, gc = x.new_id(), x.new_id()
        x.req(53, depth, struct.pack("<IIHH", pm, root, w, h), replies=0)
        x.req(55, 0, struct.pack("<III", gc, pm, 0), replies=0)
        for yy in range(h):
            for xx in range(w):
                v = (xx * 0x1357 + yy * 0x0F0F + 0x0841) & 0xFFFF if depth == 16 else (xx + yy) % 3 == 0
                x.req(56, 0, struct.pack("<III", gc, 4, int(v)), replies=0)
                x.req(64, 0, struct.pack("<IIhh", pm, gc, xx, yy), replies=0)
        masks = (0xFFFFFFFF, 0xF800, 0x0001, 0x8421, 0) if depth == 16 else (0xFFFFFFFF, 1, 0)
        for fmt in (ZPIXMAP, XYPIXMAP):
            for mask in masks:
                for (gx, gy, gw, gh) in ((0, 0, w, h), (w - 1, h - 1, 1, 1)):
                    r = x.req(73, fmt, struct.pack("<IhhHHI", pm, gx, gy, gw, gh, mask))
                    tag = f"depth {depth} {w}x{h} {'Z' if fmt == 2 else 'XY'} planes {mask:#x} rect {gx},{gy} {gw}x{gh}"
                    if isinstance(r, tuple):
                        out.append((tag, "error %d" % r[1]))
                    else:
                        out.append((tag, f"depth {r[1]} len {struct.unpack_from('<I', r, 4)[0]} data {r[32:].hex()}"))
        x.req(54, 0, struct.pack("<I", pm), replies=0)
    return out


def main():
    ref, test = run(int(sys.argv[1])), run(int(sys.argv[2]))
    diffs = 0
    for (tag, a), (_, b) in zip(ref, test):
        if a != b:
            diffs += 1
            print(f"!! {tag}\n   reference: {a}\n   woodland:  {b}")
    print(f"{len(ref)} GetImage requests, {diffs} differences")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
