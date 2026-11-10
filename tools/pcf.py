"""Minimal reader for PCF bitmap fonts (X11 Portable Compiled Format): properties,
metrics, bitmaps, encodings and accelerators. Enough for tools/gen_cursor_font.py."""
import gzip
import struct

PROPERTIES, ACCELERATORS, METRICS, BITMAPS, BDF_ENCODINGS, BDF_ACCELERATORS = 1, 2, 4, 8, 32, 256


def _fmt(fmt):
    return ">" if fmt & 4 else "<"  # PCF_BYTE_MASK: most significant byte first


def load(path):
    data = gzip.open(path).read() if path.endswith(".gz") else open(path, "rb").read()
    assert data[:4] == b"\x01fcp", "not a PCF font"
    (count,) = struct.unpack_from("<I", data, 4)
    tables = {}
    for i in range(count):
        typ, fmt, size, off = struct.unpack_from("<IIII", data, 8 + 16 * i)
        tables[typ] = off
    font = {}

    off = tables[PROPERTIES]
    (fmt,) = struct.unpack_from("<I", data, off)
    e = _fmt(fmt)
    (n,) = struct.unpack_from(e + "I", data, off + 4)
    props = [struct.unpack_from(e + "IBI", data, off + 8 + 9 * i) for i in range(n)]
    p = off + 8 + 9 * n
    p += (4 - n % 4) % 4 if n % 4 else 0
    (slen,) = struct.unpack_from(e + "I", data, p)
    strings = data[p + 4:p + 4 + slen]
    cstr = lambda o: strings[o:strings.index(b"\0", o)].decode("latin-1")
    font["properties"] = {cstr(name): (cstr(val) if is_str else val) for name, is_str, val in props}

    off = tables.get(BDF_ACCELERATORS, tables.get(ACCELERATORS))
    (fmt,) = struct.unpack_from("<I", data, off)
    e = _fmt(fmt)
    font["ascent"], font["descent"] = struct.unpack_from(e + "ii", data, off + 4 + 8)  # after 8 flag bytes

    off = tables[METRICS]
    (fmt,) = struct.unpack_from("<I", data, off)
    e = _fmt(fmt)
    metrics = []
    if fmt & 0x100:  # compressed
        (n,) = struct.unpack_from(e + "H", data, off + 4)
        for i in range(n):
            b = data[off + 6 + 5 * i:off + 11 + 5 * i]
            metrics.append(tuple(v - 0x80 for v in b))  # lsb rsb width ascent descent
    else:
        (n,) = struct.unpack_from(e + "I", data, off + 4)
        for i in range(n):
            metrics.append(struct.unpack_from(e + "hhhhhH", data, off + 8 + 12 * i)[:5])

    off = tables[BITMAPS]
    (fmt,) = struct.unpack_from("<I", data, off)
    e = _fmt(fmt)
    msbit = bool(fmt & 8)
    pad = 1 << (fmt & 3)
    (n,) = struct.unpack_from(e + "I", data, off + 4)
    offsets = struct.unpack_from(e + "%dI" % n, data, off + 8)
    sizes = struct.unpack_from(e + "4I", data, off + 8 + 4 * n)
    bits = data[off + 8 + 4 * n + 16:off + 8 + 4 * n + 16 + sizes[fmt & 3]]
    glyphs = []
    for i, (lsb, rsb, width, asc, desc) in enumerate(metrics):
        w, h = rsb - lsb, asc + desc
        stride = ((w + 7) // 8 + pad - 1) // pad * pad
        rows = []
        for y in range(h):
            row = 0
            for x in range(w):
                byte = bits[offsets[i] + y * stride + x // 8]
                on = byte >> (7 - x % 8) & 1 if msbit else byte >> (x % 8) & 1
                row |= on << x
            rows.append(row)  # bit x = pixel x (left to right)
        glyphs.append({"lsb": lsb, "rsb": rsb, "width": width, "ascent": asc, "descent": desc, "rows": rows})

    off = tables[BDF_ENCODINGS]
    (fmt,) = struct.unpack_from("<I", data, off)
    e = _fmt(fmt)
    min2, max2, min1, max1, default = struct.unpack_from(e + "5H", data, off + 4)
    ncodes = (max2 - min2 + 1) * (max1 - min1 + 1)
    idx = struct.unpack_from(e + "%dH" % ncodes, data, off + 14)
    font["first"], font["last"], font["default_char"] = min2, max2, default
    font["glyphs"] = {min2 + i: glyphs[g] for i, g in enumerate(idx) if g != 0xFFFF and min1 == max1 == 0}
    return font
