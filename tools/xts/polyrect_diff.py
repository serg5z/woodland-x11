#!/usr/bin/env python3
"""Differential PolyRectangle check: the same rectangles on a reference server (Xvfb at depth 16)
and on the Woodland host server, drawn into a depth-16 pixmap and read back with GetImage.
Systematic cases (line widths 0-5, every line style, cap and join style, dash lists and offsets,
degenerate and clipped rectangles, fill styles, plane mask) and random ones.

With GXcopy the images must match exactly. With other functions Xorg sometimes draws a pixel
twice within one rectangle (the protocol says it must not); such a case passes when the same
case drawn with GXcopy matches, i.e. both servers chose the same pixels and colours.

    Xvfb :91 -screen 0 800x480x16 -listen tcp -ac &   build/x11_host_server -d 92 -q &
    polyrect_diff.py 6091 6092 [RANDOM_CASES [SEED]]"""
import random
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from font_diff import X  # noqa: E402
from getimage_diff import root_of  # noqa: E402

W, H = 40, 32
BACK, FG, BG = 0x1234, 0xF81F, 0x07E0
RECTS = [(5, 5, 12, 8), (5, 5, 0, 0), (5, 5, 6, 0), (5, 5, 0, 6), (5, 5, 1, 1), (5, 5, 2, 1), (5, 5, 1, 2),
         (5, 5, 2, 2), (5, 5, 3, 3), (-3, -2, 9, 7), (30, 20, 20, 20), (8, 6, 20, 15)]


def systematic():
    out = []
    for lw in range(6):
        for join in range(3):
            for fn in (3, 6):
                for r in RECTS:
                    out.append(dict(lw=lw, join=join, fn=fn, rects=[r]))
    for ls in (1, 2):
        for lw in range(4):
            for cap in range(4):
                for dashes, off in (([4, 4], 0), ([4, 4], 1), ([4, 4], 6), ([1], 0), ([3, 1, 2], 0), ([3, 1, 2], 4),
                                    ([2, 5], 3), ([7], 2)):
                    for fn in (3, 6):
                        for r in ((5, 5, 12, 8), (3, 3, 4, 10), (5, 5, 0, 6), (5, 5, 6, 0), (5, 5, 0, 0), (5, 5, 1, 1)):
                            out.append(dict(lw=lw, ls=ls, cap=cap, dashes=dashes, off=off, fn=fn, rects=[r]))
        for join in range(3):
            out.append(dict(lw=3, ls=ls, join=join, rects=[(5, 5, 14, 10)]))
    out.append(dict(lw=1, ls=1, cap=3, rects=[(2, 3, 4, 10)], clip=((0, 2), [(0, 0, 6, 12)])))  # xterm's box
    for ls in range(3):
        for lw in (0, 2):
            out.append(dict(lw=lw, ls=ls, rects=[(2, 2, 30, 20)], clip=((3, 1), [(0, 0, 10, 8), (15, 10, 8, 30)])))
        out.append(dict(lw=1, ls=ls, plane=0x07E0, rects=[(4, 4, 20, 12)]))
        for fill in (1, 2, 3):
            for lw in (0, 3):
                out.append(dict(lw=lw, ls=ls, fill=fill, rects=[(4, 4, 20, 12)]))
    return out


def randomized(n, seed):
    r = random.Random(seed)
    out = []
    for _ in range(n):
        c = dict(lw=r.choice([0, 0, 1, 1, 2, 3, 4, 5, 6, 7, 9, 16, 33]), ls=r.choice([0, 1, 2]), cap=r.randrange(4),
                 join=r.randrange(3), dashes=[r.randint(1, 6) for _ in range(r.choice([1, 2, 2, 3, 4, 5]))],
                 off=r.randrange(20), fn=r.choice([3, 3, 6, 7, 1]), fill=r.choice([0, 0, 0, 1, 2, 3]),
                 rects=[(r.randint(-6, 30), r.randint(-6, 24), r.choice([0, 1, 2, 3, r.randint(0, 30)]),
                         r.choice([0, 1, 2, 3, r.randint(0, 24)])) for _ in range(r.choice([1, 1, 2, 3]))])
        if r.random() < 0.25:
            c["clip"] = ((r.randint(-3, 5), r.randint(-3, 5)),
                         [(r.randint(0, 20), r.randint(0, 15), r.randint(1, 25), r.randint(1, 20))
                          for _ in range(r.randint(1, 3))])
        out.append(c)
    return out


def run(port, cases):
    x = X(port)
    root = root_of(port)
    pm, gc, tile, stip, g = x.new_id(), x.new_id(), x.new_id(), x.new_id(), x.new_id()
    x.req(53, 16, struct.pack("<IIHH", pm, root, W, H), replies=0)
    x.req(53, 16, struct.pack("<IIHH", tile, root, 4, 3), replies=0)
    x.req(53, 1, struct.pack("<IIHH", stip, root, 5, 3), replies=0)
    for p, depth, w, h in ((tile, 16, 4, 3), (stip, 1, 5, 3)):
        x.req(55, 0, struct.pack("<III", g, p, 0), replies=0)
        for yy in range(h):
            for xx in range(w):
                v = (xx * 0x2945 + yy * 0x1234 + 0x0841) & 0xFFFF if depth == 16 else int((xx + 2 * yy) % 3 == 0)
                x.req(56, 0, struct.pack("<III", g, 4, v), replies=0)
                x.req(64, 0, struct.pack("<IIhh", p, g, xx, yy), replies=0)
        x.req(60, 0, struct.pack("<I", g), replies=0)
    images = []
    for c in cases:
        x.req(55, 0, struct.pack("<IIII", gc, pm, 4, BACK), replies=0)
        x.req(70, 0, struct.pack("<IIhhHH", pm, gc, 0, 0, W, H), replies=0)
        vals = sorted([(0x1, c.get("fn", 3)), (0x2, c.get("plane", 0xFFFFFFFF)), (0x4, FG), (0x8, BG), (0x10, c["lw"]),
                       (0x20, c.get("ls", 0)), (0x40, c.get("cap", 1)), (0x80, c.get("join", 0)), (0x100, c.get("fill", 0)),
                       (0x400, tile), (0x800, stip), (0x1000, 1), (0x2000, 2)])
        mask = sum(b for b, _ in vals)
        x.req(56, 0, struct.pack("<II", gc, mask) + b"".join(struct.pack("<I", v) for _, v in vals), replies=0)
        d = c.get("dashes", [4, 4])
        x.req(58, 0, struct.pack("<IHH", gc, c.get("off", 0), len(d)) + bytes(d), replies=0)
        if "clip" in c:
            (cx, cy), rs = c["clip"]
            x.req(59, 0, struct.pack("<Ihh", gc, cx, cy) + b"".join(struct.pack("<hhHH", *q) for q in rs), replies=0)
        e = x.req(67, 0, struct.pack("<II", pm, gc) + b"".join(struct.pack("<hhHH", *q) for q in c["rects"]), replies=0)
        img = x.req(73, 2, struct.pack("<IhhHHI", pm, 0, 0, W, H, 0xFFFFFFFF))
        images.append(("error %d" % e[1]) if e else img[32:32 + W * H * 2])
        x.req(60, 0, struct.pack("<I", gc), replies=0)
    return images


def describe(c):
    keys = ("lw", "ls", "cap", "join", "dashes", "off", "fn", "fill", "plane", "rects", "clip")
    return " ".join(f"{k}={c[k]}" for k in keys if k in c)


def main():
    ref_port, test_port = int(sys.argv[1]), int(sys.argv[2])
    cases = systematic()
    if len(sys.argv) > 3:
        cases += randomized(int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    ref, test = run(ref_port, cases), run(test_port, cases)
    retry = [i for i, c in enumerate(cases) if ref[i] != test[i] and c.get("fn", 3) != 3]
    as_copy = [dict(cases[i], fn=3) for i in retry]
    ref2, test2 = run(ref_port, as_copy), run(test_port, as_copy)
    copy_ok = {i: ref2[k] == test2[k] for k, i in enumerate(retry)}
    exact = twice = 0
    bad = []
    for i, c in enumerate(cases):
        if ref[i] == test[i]:
            exact += 1
        elif copy_ok.get(i):
            twice += 1
        else:
            bad.append(describe(c))
    print(f"{len(cases)} cases: {exact} identical, {twice} differ only where Xorg draws a pixel twice, "
          f"{len(bad)} differ")
    for b in bad[:20]:
        print("  DIFFER", b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
