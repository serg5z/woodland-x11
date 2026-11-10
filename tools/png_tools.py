"""Read the host server's framebuffer PNGs and write compressed crops (stdlib only)."""
import struct
import zlib


def read_png(path):
    """-> (width, height, rows) with rows[y][x] = (r, g, b). 8-bit RGB, filter 0 only (as
    written by host/png_write.c)."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat = 8, b""
    w = h = 0
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + 3 * w
    rows = []
    for y in range(h):
        line = raw[y * stride + 1:(y + 1) * stride]
        rows.append([tuple(line[3 * x:3 * x + 3]) for x in range(w)])
    return w, h, rows


def write_png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def crop(rows, x0, y0, x1, y1):
    return [row[x0:x1] for row in rows[y0:y1]]


def bbox(rows, pred):
    ys = [y for y, row in enumerate(rows) if any(pred(p) for p in row)]
    if not ys:
        return None
    xs = [x for row in rows for x, p in enumerate(row) if pred(p)]
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1
