#!/usr/bin/env python3
"""Compiles PCF fonts into x11_font tables (core/x11_font.h) for the host test server.

    gen_pcf_fonts.py OUT.c FONTDIR

FONTDIR has a fonts.dir (file name -> font name), as XTS's xts5/fonts after its build. Each
.pcf (or .pcf.gz) is read the way X.Org's libXfont serves it, so that QueryFont,
ListFontsWithInfo and QueryTextExtents match an X.Org server with the same font:
- properties from the PCF properties table (strings become atoms, in table order);
- encoding ranges and the default character from the BDF encodings table;
- font ascent/descent, draw direction and the reported min/max bounds (the ink bounds when
  present) from the BDF accelerators (or the plain accelerators);
- per-character metrics: the ink metrics when the font has them, else the bitmap metrics;
  characters without a glyph get all-zero metrics ("does not exist");
- bitmaps cropped to those metrics: rows of (rsb - lsb + 7) / 8 bytes, bit 7 leftmost,
  ascent + descent rows, so the same table drives QueryFont and drawing.
The output defines `x11_test_fonts` / `x11_test_font_count` for builds with -DX11_TEST_FONTS.
"""
import gzip
import os
import struct
import sys

PROPERTIES, ACCELERATORS, METRICS, BITMAPS, INK_METRICS, BDF_ENCODINGS = 1, 2, 4, 8, 16, 32
BDF_ACCELERATORS = 256
COMPRESSED_METRICS, ACCEL_W_INKBOUNDS = 0x100, 0x100


class Table:
    def __init__(self, data, offset):
        self.fmt = struct.unpack_from("<i", data, offset)[0]
        self.e = ">" if self.fmt & 4 else "<"  # byte order of the table's data
        self.d, self.o = data, offset + 4

    def take(self, code):
        v = struct.unpack_from(self.e + code, self.d, self.o)
        self.o += struct.calcsize(self.e + code)
        return v if len(v) > 1 else v[0]

    def metric(self, compressed):
        if compressed:
            return tuple(b - 0x80 for b in self.take("5B"))
        return self.take("6h")[:5]


def read_pcf(path):
    data = gzip.open(path).read() if path.endswith(".gz") else open(path, "rb").read()
    assert data[:4] == b"\x01fcp", path
    count = struct.unpack_from("<i", data, 4)[0]
    tables = {}
    for i in range(count):
        typ, fmt, size, off = struct.unpack_from("<iiii", data, 8 + 16 * i)
        tables[typ] = off
    font = {}
    # properties
    t = Table(data, tables[PROPERTIES])
    n = t.take("i")
    raw = [t.take("i") for _ in range(0)]
    props = []
    for _ in range(n):
        name_off = t.take("i")
        is_str = t.take("B")
        value = t.take("i")
        raw.append((name_off, is_str, value))
    t.o += (4 - (n & 3)) & 3
    t.take("i")  # string size
    strings = t.o

    def cstr(o):
        end = data.index(b"\0", strings + o)
        return data[strings + o:end].decode("latin-1")
    for name_off, is_str, value in raw:
        props.append((cstr(name_off), cstr(value) if is_str else value))
    font["props"] = props
    # accelerators (the BDF ones when present: X.Org prefers them)
    t = Table(data, tables.get(BDF_ACCELERATORS, tables[ACCELERATORS]))
    flags = t.take("8B")
    font["direction"] = flags[6]
    font["ascent"], font["descent"], _ = t.take("3i")
    minb, maxb = t.metric(False), t.metric(False)
    if t.fmt & ACCEL_W_INKBOUNDS:
        minb, maxb = t.metric(False), t.metric(False)
    font["min_bounds"], font["max_bounds"] = minb, maxb
    # encodings
    t = Table(data, tables[BDF_ENCODINGS])
    b2lo, b2hi, b1lo, b1hi, default = t.take("5h")
    slots = (b1hi - b1lo + 1) * (b2hi - b2lo + 1)
    glyph_of = [t.take("H") for _ in range(slots)]
    font.update(b2lo=b2lo, b2hi=b2hi, b1lo=b1lo, b1hi=b1hi, default=default & 0xFFFF, glyph_of=glyph_of)
    # bitmap metrics, ink metrics
    def metrics_table(off):
        t = Table(data, off)
        comp = bool(t.fmt & COMPRESSED_METRICS)
        n = t.take("h" if comp else "i")
        return [t.metric(comp) for _ in range(n)]
    bitmap_metrics = metrics_table(tables[METRICS])
    ink_metrics = metrics_table(tables[INK_METRICS]) if INK_METRICS in tables else bitmap_metrics
    # bitmaps: glyph padding, bit order and scan unit from the format
    t = Table(data, tables[BITMAPS])
    n = t.take("i")
    offsets = [t.take("i") for _ in range(n)]
    sizes = t.take("4i")
    pad_bytes = 1 << (t.fmt & 3)
    msb_bit = bool(t.fmt & 8)
    unit = 1 << ((t.fmt >> 4) & 3)
    msb_byte = bool(t.fmt & 4)
    base = t.o

    def pixel(g, x, y):
        lsb, rsb, _, asc, desc = bitmap_metrics[g]
        w = rsb - lsb
        if not (0 <= x < w and 0 <= y < asc + desc):
            return 0
        stride = ((w + pad_bytes * 8 - 1) // (pad_bytes * 8)) * pad_bytes
        byte = x // 8
        if unit > 1 and msb_byte != msb_bit:  # bytes within a scan unit are swapped
            byte = (byte // unit) * unit + (unit - 1 - byte % unit)
        b = data[base + offsets[g] + y * stride + byte]
        return b >> (7 - x % 8) & 1 if msb_bit else b >> (x % 8) & 1
    glyphs = []
    for g in range(n):
        ink = ink_metrics[g]
        bm = bitmap_metrics[g]
        lsb, rsb, width, asc, desc = ink
        w, h = max(rsb - lsb, 0), max(asc + desc, 0)
        rows = bytearray()
        for y in range(h):
            row = bytearray((w + 7) // 8)
            for x in range(w):
                if pixel(g, x + lsb - bm[0], y + bm[3] - asc):
                    row[x // 8] |= 0x80 >> (x % 8)
            rows += row
        glyphs.append((ink, bytes(rows)))
    font["glyphs"] = glyphs
    del sizes
    return font


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit(out, ident, xname, f):
    cols = f["b2hi"] - f["b2lo"] + 1
    metrics, offsets, bits = [], [], bytearray()
    for g in f["glyph_of"]:
        m, data = f["glyphs"][g] if g != 0xFFFF else ((0, 0, 0, 0, 0), b"")
        if not any(m):
            data = b""
        metrics.append(m)
        offsets.append(len(bits))
        bits += data
    slots = len(metrics)
    exist = sum(1 for m in metrics if any(m))
    out.append(f"/* {xname}: {exist} characters, byte1 {f['b1lo']}..{f['b1hi']}, byte2 {f['b2lo']}..{f['b2hi']} */")
    out.append(f"static const x11_glyph_metrics {ident}_m[{slots}] = {{")
    for i in range(0, slots, 4):
        out.append("    " + " ".join("{%d, %d, %d, %d, %d}," % m for m in metrics[i:i + 4]))
    out.append("};")
    out.append(f"static const uint32_t {ident}_o[{slots}] = {{")
    for i in range(0, slots, 12):
        out.append("    " + " ".join("%d," % o for o in offsets[i:i + 12]))
    out.append("};")
    blob = bytes(bits) or b"\0"
    out.append(f"static const uint8_t {ident}_b[{len(blob)}] = {{")
    for i in range(0, len(blob), 16):
        out.append("    " + " ".join("0x%02x," % b for b in blob[i:i + 16]))
    out.append("};")
    props = f["props"] or [("", 0)]
    out.append(f"static const x11_font_prop {ident}_p[{len(props)}] = {{")
    for name, value in props:
        if isinstance(value, str):
            out.append(f"    {{{c_string(name)}, {c_string(value)}, 0}},")
        else:
            out.append(f"    {{{c_string(name)}, 0, {value}}},")
    out.append("};")
    out.append(f"static const x11_font {ident} = {{")
    out.append(f"    {c_string(xname)}, 0, 0, {f['b2lo']}, {f['b2hi']}, {f['default']}, {f['ascent']}, {f['descent']},")
    out.append("    {%d, %d, %d, %d, %d}, {%d, %d, %d, %d, %d}," % (f["min_bounds"] + f["max_bounds"]))
    out.append(f"    {ident}_m, 0, 0, 0, 0, {f['b1lo']}, {f['b1hi']}, {ident}_o, {ident}_b, {ident}_p, "
               f"{len(f['props'])}, {f['direction']}}};")
    out.append("")


def main():
    out_path, font_dir = sys.argv[1], sys.argv[2]
    with open(os.path.join(font_dir, "fonts.dir")) as fd:
        entries = [line.split(None, 1) for line in fd.read().splitlines()[1:] if line.strip()]
    out = ["/* Generated by tools/gen_pcf_fonts.py from " + os.path.basename(os.path.abspath(font_dir)) +
           " -- do not edit. Test fonts for the host server only (-DX11_TEST_FONTS). */",
           '#include "x11_font.h"', ""]
    idents = []
    for i, (fname, xname) in enumerate(entries):
        ident = "test_font_%d" % i
        emit(out, ident, xname.strip(), read_pcf(os.path.join(font_dir, fname)))
        idents.append(ident)
    out.append("const x11_font *const x11_test_fonts[] = {" + ", ".join("&" + i for i in idents) + "};")
    out.append(f"const int x11_test_font_count = {len(idents)};")
    with open(out_path, "w") as fo:
        fo.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
