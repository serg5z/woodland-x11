"""A few core X11 requests over a raw socket, for acceptance tests (LSB-first client).
Replies are matched by sequence number; events and errors that arrive meanwhile are kept in
`events` and `errors`. No dependencies besides x11_client.connect."""
from __future__ import annotations

import socket
import struct

from x11_client import connect

ROOT = 0x26


class Raw:
    def __init__(self, port: int, host: str = "127.0.0.1", auth_name: bytes = b"", auth_data: bytes = b""):
        self.sock, setup = connect(port, auth_name=auth_name, auth_data=auth_data, host=host)
        assert setup["status"] == "success", setup
        self.base = setup["resource_id_base"]
        self.next_id = 1
        self.seq = 0
        self.events: list[bytes] = []
        self.errors: list[bytes] = []
        self.buf = b""

    def new_id(self) -> int:
        self.next_id += 1
        return self.base + self.next_id

    def close(self):
        self.sock.close()

    # ---- wire ----
    def send(self, op: int, data1: int, body: bytes = b"") -> int:
        n = 4 + len(body)
        assert n % 4 == 0
        self.sock.sendall(struct.pack("<BBH", op, data1, n // 4) + body)
        self.seq = (self.seq + 1) & 0xFFFF
        return self.seq

    def _message(self) -> bytes:
        while len(self.buf) < 32:
            part = self.sock.recv(65536)
            if not part:
                raise ConnectionError("server closed the connection")
            self.buf += part
        n = 32 + (struct.unpack_from("<I", self.buf, 4)[0] * 4 if self.buf[0] == 1 else 0)
        while len(self.buf) < n:
            part = self.sock.recv(65536)
            if not part:
                raise ConnectionError("server closed the connection")
            self.buf += part
        m, self.buf = self.buf[:n], self.buf[n:]
        return m

    def reply(self, seq: int) -> bytes:
        while True:
            m = self._message()
            if m[0] == 1 and struct.unpack_from("<H", m, 2)[0] == seq:
                return m
            if m[0] == 0:
                self.errors.append(m)
                if struct.unpack_from("<H", m, 2)[0] == seq:
                    raise RuntimeError(f"X error {m[1]} for request {m[10]}")
            else:
                self.events.append(m)

    def sync(self):
        self.get_input_focus()

    def poll_events(self, timeout: float = 0.2) -> list[bytes]:
        """Events received so far, after waiting up to `timeout` for more."""
        self.sock.settimeout(timeout)
        try:
            while True:
                m = self._message()
                (self.errors if m[0] == 0 else self.events).append(m)
        except (socket.timeout, TimeoutError):
            pass
        finally:
            self.sock.settimeout(5)
        out, self.events = self.events, []
        return out

    # ---- requests ----
    def create_window(self, parent: int, x: int, y: int, w: int, h: int, events: int = 0, bg: int = 0xFFFF,
                      override: bool = False) -> int:
        wid = self.new_id()
        mask, values = 1 << 1, [bg]
        if override:
            mask |= 1 << 9
            values.append(1)
        if events:
            mask |= 1 << 11
            values.append(events)
        body = struct.pack("<IIhhHHHHII", wid, parent, x, y, w, h, 0, 1, 0, mask) + struct.pack(f"<{len(values)}I", *values)
        self.send(1, 0, body)
        return wid

    def select(self, wid: int, events: int):
        self.send(2, 0, struct.pack("<III", wid, 1 << 11, events))

    def map(self, wid: int):
        self.send(8, 0, struct.pack("<I", wid))

    def create_gc(self, drawable: int, foreground: int) -> int:
        gc = self.new_id()
        self.send(55, 0, struct.pack("<IIII", gc, drawable, 1 << 2, foreground))
        return gc

    def fill_rects(self, drawable: int, gc: int, rects: list[tuple[int, int, int, int]]):
        body = struct.pack("<II", drawable, gc) + b"".join(struct.pack("<hhHH", *r) for r in rects)
        self.send(70, 0, body)

    def clear(self, wid: int):
        self.send(61, 0, struct.pack("<IhhHH", wid, 0, 0, 0, 0))

    def query_tree(self, wid: int) -> tuple[int, list[int]]:
        r = self.reply(self.send(15, 0, struct.pack("<I", wid)))
        n = struct.unpack_from("<H", r, 16)[0]
        return struct.unpack_from("<I", r, 12)[0], list(struct.unpack_from(f"<{n}I", r, 32))

    def geometry(self, wid: int) -> tuple[int, int, int, int, int]:
        r = self.reply(self.send(14, 0, struct.pack("<I", wid)))
        return struct.unpack_from("<hhHHH", r, 12)

    def attributes(self, wid: int) -> dict:
        r = self.reply(self.send(3, 0, struct.pack("<I", wid)))
        return {"map_state": r[26], "override_redirect": r[27], "class": struct.unpack_from("<H", r, 12)[0]}

    def translate(self, src: int, dst: int, x: int = 0, y: int = 0) -> tuple[int, int, int]:
        r = self.reply(self.send(40, 0, struct.pack("<IIhh", src, dst, x, y)))
        child, tx, ty = struct.unpack_from("<Ihh", r, 8)
        return child, tx, ty

    def query_pointer(self, wid: int = ROOT) -> dict:
        r = self.reply(self.send(38, 0, struct.pack("<I", wid)))
        root, child, rx, ry, wx, wy, mask = struct.unpack_from("<IIhhhhH", r, 8)
        return {"child": child, "root_x": rx, "root_y": ry, "win_x": wx, "win_y": wy, "mask": mask}

    def get_input_focus(self) -> int:
        r = self.reply(self.send(43, 0))
        return struct.unpack_from("<I", r, 8)[0]

    def grab_pointer(self, wid: int, events: int, owner: int = 0, sync: bool = False) -> int:
        body = struct.pack("<IHBBIII", wid, events, 0 if sync else 1, 1, 0, 0, 0)
        return self.reply(self.send(26, owner, body))[1]

    def get_property(self, wid: int, atom: int, type_: int = 0, length: int = 256) -> bytes:
        r = self.reply(self.send(20, 0, struct.pack("<IIIII", wid, atom, type_, 0, length)))
        fmt, n = r[1], struct.unpack_from("<I", r, 16)[0]
        return r[32:32 + n * (fmt // 8)]

    def intern(self, name: str) -> int:
        b = name.encode()
        body = struct.pack("<HH", len(b), 0) + b + b"\0" * (-len(b) % 4)
        return struct.unpack_from("<I", self.reply(self.send(16, 1, body)), 8)[0]


def decode_event(m: bytes) -> dict:
    """Pointer and crossing events (codes 4..8) decoded; others give the code only."""
    code = m[0] & 0x7F
    d = {"code": code, "detail": m[1]}
    if 4 <= code <= 8:
        (d["time"], d["root"], d["window"], d["child"], d["root_x"], d["root_y"], d["x"], d["y"],
         d["state"]) = struct.unpack_from("<IIIIhhhhH", m, 4)
        d["mode"], d["flags"] = m[30], m[31]
    return d
