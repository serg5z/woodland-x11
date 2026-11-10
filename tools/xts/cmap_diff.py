#!/usr/bin/env python3
"""Differential colormap check: the same colour requests on a reference server (Xvfb at depth
16, TrueColor default visual) and on the Woodland host server; prints each outcome (reply,
no error, or the error code and bad value) side by side and the differences.

    cmap_diff.py REF_PORT TEST_PORT"""
import socket
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from font_diff import X  # noqa: E402


def default_colormap(port):
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(b"l\0\x0b\0\0\0\0\0\0\0\0\0")
    h = s.recv(8)
    n = struct.unpack("<H", h[6:8])[0] * 4
    d = b""
    while len(d) < n:
        d += s.recv(n - len(d))
    o = 32 + ((struct.unpack_from("<H", d, 16)[0] + 3) & ~3) + 8 * d[21]
    s.close()
    return struct.unpack_from("<I", d, o + 4)[0]


def run(port):
    x = X(port)
    cm = default_colormap(port)
    out = []

    def rec(tag, r, reply_fields=None):
        if isinstance(r, tuple):
            out.append((tag, "error %d" % r[1]))
        elif r is None:
            out.append((tag, "ok"))
        else:
            out.append((tag, "reply " + (reply_fields(r) if reply_fields else "")))
        return r

    pix = lambda r: "pixel %#x" % struct.unpack_from("<I", r, 16)[0]
    rec("AllocColorCells 1", x.req(86, 0, struct.pack("<IHH", cm, 1, 0)))
    rec("AllocColorCells 0", x.req(86, 0, struct.pack("<IHH", cm, 0, 0)))
    rec("AllocColorCells contiguous 2", x.req(86, 2, struct.pack("<IHH", cm, 1, 0)))
    rec("AllocColorCells bad cmap", x.req(86, 0, struct.pack("<IHH", 0x1234567, 1, 0)))
    rec("AllocColorPlanes", x.req(87, 0, struct.pack("<IHHHH", cm, 1, 1, 1, 1)))
    rec("AllocColorPlanes 0", x.req(87, 0, struct.pack("<IHHHH", cm, 0, 1, 1, 1)))
    rec("StoreColors 1 item", x.req(89, 0, struct.pack("<IIHHHBB", cm, 0, 1, 2, 3, 7, 0), replies=0))
    rec("StoreColors no items", x.req(89, 0, struct.pack("<I", cm), replies=0))
    rec("StoreColors bad cmap", x.req(89, 0, struct.pack("<I", 0x1234567), replies=0))
    rec("StoreNamedColor red", x.req(90, 7, struct.pack("<IIHH", cm, 0, 3, 0) + b"red", replies=0))
    rec("StoreNamedColor unknown", x.req(90, 7, struct.pack("<IIHH", cm, 0, 5, 0) + b"nosuc", replies=0))
    rec("FreeColors never allocated", x.req(88, 0, struct.pack("<III", cm, 0, 0x1234), replies=0))
    r = rec("AllocColor", x.req(84, 0, struct.pack("<IHHHH", cm, 0xFFFF, 0x8000, 0, 0)), pix)
    px = struct.unpack_from("<I", r, 16)[0]
    rec("FreeColors it", x.req(88, 0, struct.pack("<III", cm, 0, px), replies=0))
    rec("FreeColors it again", x.req(88, 0, struct.pack("<III", cm, 0, px), replies=0))
    rec("AllocColor twice", x.req(84, 0, struct.pack("<IHHHH", cm, 0xFFFF, 0x8000, 0, 0)), pix)
    x.req(84, 0, struct.pack("<IHHHH", cm, 0xFFFF, 0x8000, 0, 0))
    rec("FreeColors 1 of 2", x.req(88, 0, struct.pack("<III", cm, 0, px), replies=0))
    rec("FreeColors 2 of 2", x.req(88, 0, struct.pack("<III", cm, 0, px), replies=0))
    rec("FreeColors 0x10000", x.req(88, 0, struct.pack("<III", cm, 0, 0x10000), replies=0))
    x.req(84, 0, struct.pack("<IHHHH", cm, 0xFFFF, 0x8000, 0, 0))
    rec("FreeColors plane mask 1", x.req(88, 0, struct.pack("<III", cm, 1, px), replies=0))
    rec("FreeColors after mask", x.req(88, 0, struct.pack("<III", cm, 0, px), replies=0))
    r = rec("AllocNamedColor red", x.req(85, 0, struct.pack("<IHH", cm, 3, 0) + b"red"),
            lambda r: "pixel %#x" % struct.unpack_from("<I", r, 8)[0])
    rec("FreeColors named", x.req(88, 0, struct.pack("<III", cm, 0, struct.unpack_from("<I", r, 8)[0]), replies=0))
    x.req(84, 0, struct.pack("<IHHHH", cm, 0xFFFF, 0x8000, 0, 0))
    mid = x.new_id()
    rec("CopyColormapAndFree", x.req(80, 0, struct.pack("<II", mid, cm), replies=0))
    rec("FreeColors in source", x.req(88, 0, struct.pack("<III", cm, 0, px), replies=0))
    rec("FreeColors in copy", x.req(88, 0, struct.pack("<III", mid, 0, px), replies=0))
    rec("CopyColormapAndFree ID in use", x.req(80, 0, struct.pack("<II", mid, cm), replies=0))
    rec("CopyColormapAndFree bad source", x.req(80, 0, struct.pack("<II", x.new_id(), 0x1234567), replies=0))
    rec("FreeColormap copy", x.req(79, 0, struct.pack("<I", mid), replies=0))
    rec("QueryColors 0x10000", x.req(91, 0, struct.pack("<II", cm, 0x10000)))
    rec("QueryColors 0xF800", x.req(91, 0, struct.pack("<II", cm, 0xF800)),
        lambda r: "rgb %04x %04x %04x" % struct.unpack_from("<HHH", r, 32))
    return out


def main():
    ref, test = run(int(sys.argv[1])), run(int(sys.argv[2]))
    diffs = 0
    for (tag, a), (_, b) in zip(ref, test):
        mark = "  " if a == b else "!!"
        diffs += a != b
        print(f"{mark} {tag:32} reference: {a:24} woodland: {b}")
    print(f"{len(ref)} requests, {diffs} differences")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
