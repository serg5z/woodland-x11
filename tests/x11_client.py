#!/usr/bin/env python3
"""Minimal X11 test client: connection setup in either byte order, decoded independently of
the C server code, straight from the protocol specification (section 8 and the
"Connection Setup" encoding). No dependencies.

    python3 x11_client.py [--display 99] [--msb] [--auth-name NAME --auth-hex HEX]
"""
from __future__ import annotations

import argparse
import json
import socket
import struct


def pad4(n: int) -> int:
    return (n + 3) & ~3


def setup_request(msb: bool, major: int = 11, minor: int = 0, auth_name: bytes = b"",
                  auth_data: bytes = b"") -> bytes:
    e = ">" if msb else "<"
    head = (b"B" if msb else b"l") + b"\0" + struct.pack(e + "HHHH", major, minor, len(auth_name), len(auth_data)) + b"\0\0"
    return (head + auth_name + b"\0" * (pad4(len(auth_name)) - len(auth_name))
            + auth_data + b"\0" * (pad4(len(auth_data)) - len(auth_data)))


class Reader:
    def __init__(self, data: bytes, msb: bool):
        self.d, self.o, self.e = data, 0, ">" if msb else "<"

    def take(self, fmt: str):
        v = struct.unpack_from(self.e + fmt, self.d, self.o)
        self.o += struct.calcsize(self.e + fmt)
        return v if len(v) > 1 else v[0]

    def raw(self, n: int) -> bytes:
        b = self.d[self.o:self.o + n]
        self.o += n
        return b


def recv_exact(s: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        part = s.recv(n - len(buf))
        if not part:
            raise ConnectionError(f"closed after {len(buf)} of {n} bytes")
        buf += part
    return buf


def read_setup_reply(s: socket.socket, msb: bool) -> dict:
    head = recv_exact(s, 8)
    status = head[0]
    e = ">" if msb else "<"
    length = struct.unpack(e + "H", head[6:8])[0]
    body = recv_exact(s, length * 4)
    if status == 0:
        n = head[1]
        major, minor = struct.unpack(e + "HH", head[2:6])
        return {"status": "failed", "reason": body[:n].decode("latin-1"), "version": [major, minor],
                "length_units": length, "padding_zero": body[n:] == b"\0" * (len(body) - n)}
    if status == 2:
        return {"status": "authenticate", "reason": body.rstrip(b"\0").decode("latin-1")}
    assert status == 1, status
    r = Reader(body, msb)
    out = {"status": "success", "version": list(struct.unpack(e + "HH", head[2:6])), "length_units": length}
    out["release"], out["resource_id_base"], out["resource_id_mask"], out["motion_buffer"] = r.take("IIII")
    vlen, out["max_request_units"] = r.take("HH")
    nscreens, nformats = r.take("BB")
    (out["image_byte_order"], out["bitmap_bit_order"], out["scanline_unit"], out["scanline_pad"],
     out["min_keycode"], out["max_keycode"]) = r.take("BBBBBB")
    r.raw(4)
    out["vendor"] = r.raw(vlen).decode("latin-1")
    r.raw(pad4(vlen) - vlen)
    out["formats"] = []
    for _ in range(nformats):
        depth, bpp, spad = r.take("BBB")
        r.raw(5)
        out["formats"].append({"depth": depth, "bpp": bpp, "scanline_pad": spad})
    out["screens"] = []
    for _ in range(nscreens):
        sc = {}
        (sc["root"], sc["default_colormap"], sc["white_pixel"], sc["black_pixel"],
         sc["input_masks"]) = r.take("IIIII")
        sc["width_px"], sc["height_px"], sc["width_mm"], sc["height_mm"] = r.take("HHHH")
        sc["min_maps"], sc["max_maps"] = r.take("HH")
        sc["root_visual"] = r.take("I")
        sc["backing_stores"], sc["save_unders"], sc["root_depth"], ndepths = r.take("BBBB")
        sc["depths"] = []
        for _ in range(ndepths):
            depth = r.take("B")
            r.raw(1)
            nvis = r.take("H")
            r.raw(4)
            vis = []
            for _ in range(nvis):
                vid, cls, bits, entries, rm, gm, bm = r.take("IBBHIII")
                r.raw(4)
                vis.append({"id": vid, "class": cls, "bits_per_rgb": bits, "colormap_entries": entries,
                            "masks": [rm, gm, bm]})
            sc["depths"].append({"depth": depth, "visuals": vis})
        out["screens"].append(sc)
    out["trailing_bytes"] = len(body) - r.o
    return out


def connect(port: int, msb: bool = False, auth_name: bytes = b"", auth_data: bytes = b"",
            major: int = 11, chunks: int = 0, host: str = "127.0.0.1") -> tuple[socket.socket, dict]:
    s = socket.create_connection((host, port), timeout=5)
    req = setup_request(msb, major, 0, auth_name, auth_data)
    if chunks:  # send in pieces to exercise the server's partial-read handling
        for i in range(0, len(req), chunks):
            s.sendall(req[i:i + chunks])
    else:
        s.sendall(req)
    return s, read_setup_reply(s, msb)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--display", type=int, default=99)
    ap.add_argument("--msb", action="store_true", help="MSB-first (big-endian) client")
    ap.add_argument("--auth-name", default="")
    ap.add_argument("--auth-hex", default="")
    a = ap.parse_args()
    s, rep = connect(6000 + a.display, a.msb, a.auth_name.encode(), bytes.fromhex(a.auth_hex))
    s.close()
    print(json.dumps(rep, indent=2))
    return 0 if rep["status"] == "success" else 1


if __name__ == "__main__":
    raise SystemExit(main())
