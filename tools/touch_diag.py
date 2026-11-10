#!/usr/bin/env python3
"""Multi-touch diagnostics display for the Woodland X11 panel (P6).

A plain X11 client (raw protocol, no libraries) for the touch-diagnostics test image
(-DX11_TOUCH_DIAG). It polls the root property _WOODLAND_DIAG, takes the tracker's "mt.*"
lines, and draws a full-screen window on the panel:

  - one marker per contact; the marker's SHAPE tells the tracker slot (circle, square,
    triangle, diamond, X), a crosshair marks the exact point, and a label gives the slot,
    the controller's track ID, x,y, size and the state (NEW / MOVE / STILL / UP);
  - a header with the current and maximum contact count and the tracker's counters;
  - the chain's frame counters and the last contact transitions (BEGIN / END / LOST, with
    the frame number: "tc.ev*" lines), newest first;
  - a short trail behind each contact (its last --trail positions, 0: none).

    tools/touch_diag.py --display PANEL_IP:0 [--hz 20] [--trail 24] [--log touch.jsonl] [--seconds S]
    tools/touch_diag.py --display 127.0.0.1:177 --no-auth --fake diag.txt --seconds 1   (tests)

--log writes one JSON line per change (time, seq, contacts, counters) for the record.
Nothing is drawn by the firmware itself; quitting (Ctrl-C) closes the window.
"""
from __future__ import annotations

import argparse
import json
import os
import select
import socket
import struct
import subprocess
import sys
import time

BG, FG, DIM = 0x2104, 0xFFFF, 0x8410                     # RGB565: dark grey, white, grey
SLOT_COLOURS = (0xF800, 0x07E0, 0x07FF, 0xFFE0, 0xF81F)  # red, green, cyan, yellow, magenta
SHAPES = ("circle", "square", "triangle", "diamond", "x")
R = 38                                                   # marker radius


def cookie_for(display: str) -> bytes:
    out = subprocess.run(["xauth", "list", display], capture_output=True, text=True, check=False).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] == "MIT-MAGIC-COOKIE-1":
            return bytes.fromhex(parts[2])
    raise SystemExit(f"no MIT-MAGIC-COOKIE-1 for {display} in xauth")


def pad(b: bytes) -> bytes:
    return b + b"\0" * (-len(b) % 4)


def req(op: int, data1: int = 0, body: bytes = b"") -> bytes:
    return struct.pack("<BBH", op, data1, (4 + len(body)) // 4) + body


# ---- the model: diagnostics text -> contacts and counters; contacts -> drawing ------------

def parse(text: str) -> tuple[list[dict], dict]:
    """-> (contacts, counters); counters holds the tracker's mt.* values by name and the
    gesture recogniser's g.* values under "g.<name>" (events: g.evN and g.update)."""
    contacts, counters = [], {}
    for line in text.splitlines():
        key, sep, val = line.partition("=")
        if sep and key.startswith(("g.", "gate.", "tc.")):
            counters[key] = val
            continue
        if not sep or not key.startswith("mt."):
            continue
        name = key[3:]
        if name.startswith("c") and name[1:].isdigit():
            f = val.split(",")
            if len(f) == 5:
                contacts.append({"slot": int(name[1:]), "id": int(f[0]), "x": int(f[1]), "y": int(f[2]),
                                 "size": int(f[3]), "state": f[4]})
        else:
            counters[name] = val
    contacts.sort(key=lambda c: c["slot"])
    return contacts, counters


def shape_ops(slot: int, x: int, y: int) -> list[tuple]:
    """Drawing operations for one marker: (kind, args). Shapes, not colour, identify slots."""
    s = SHAPES[slot % len(SHAPES)]
    if s == "circle":
        ops = [("arc", (x - R, y - R, 2 * R, 2 * R))]
    elif s == "square":
        ops = [("rect", (x - R, y - R, 2 * R, 2 * R))]
    elif s == "triangle":
        ops = [("lines", ((x, y - R), (x + R, y + R), (x - R, y + R), (x, y - R)))]
    elif s == "diamond":
        ops = [("lines", ((x, y - R), (x + R, y), (x, y + R), (x - R, y), (x, y - R)))]
    else:
        ops = [("segments", ((x - R, y - R, x + R, y + R), (x - R, y + R, x + R, y - R)))]
    ops.append(("segments", ((x - 8, y, x + 8, y), (x, y - 8, x, y + 8))))  # the exact point
    return ops


def label(c: dict) -> str:
    return f"#{c['slot']} {SHAPES[c['slot'] % len(SHAPES)]} id{c['id']} {c['x']},{c['y']} s{c['size']} {c['state']}"


def gesture_lines(counters: dict) -> list[str]:
    """The recogniser's counters, its latest UPDATE and its last events (newest first)."""
    if "g.pairs" not in counters:
        return []
    g = counters.get
    out = [f"gestures: pairs {g('g.pairs')}  scroll v {g('g.scroll_v')} h {g('g.scroll_h')}  pan {g('g.pan')}  "
           f"pinch {g('g.pinch')}  taps {g('g.taps')}  ends {g('g.ends')}  cancels {g('g.cancels')}"]
    if "g.x11_button4" in counters:
        out.append(f"X11: wheel up {g('g.x11_button4')}  wheel down {g('g.x11_button5')}  right click {g('g.x11_button3')}  "
                   f"held {g('g.x11_deferred')}  dropped {g('g.x11_dropped')}")
    if "gate.taps" in counters:
        still = f" (still {g('gate.still_ms')} ms, moved {g('gate.move_px')} px)" if "gate.still_ms" in counters else ""
        out.append(f"pointer gate {g('gate.hold_ms')} ms{still}: taps {g('gate.taps')}  presses {g('gate.commits')}  "
                   f"kept from gestures {g('gate.discards')}  latency {g('gate.latency_ms_last')} ms "
                   f"(max {g('gate.latency_ms_max')})")

    def fmt(v):
        f = v.split(",")
        if len(f) != 9:
            return v
        t, ev, kind, cx, cy, tx, ty, scale, steps = f
        extra = f" scale {scale} steps {steps}" if kind == "pinch" else f" total {tx},{ty} steps {steps}"
        return f"{ev} {kind} at {cx},{cy}{extra if ev in ('update', 'end') else ''}"

    if "g.update" in counters:
        out.append("now: " + fmt(counters["g.update"]))
    evs = sorted((int(k[4:]), v) for k, v in counters.items() if k.startswith("g.ev") and k[4:].isdigit())
    out += [fmt(v) for _, v in reversed(evs[-4:])]
    return out


def transitions(counters: dict) -> list[dict]:
    """The chain's last transitions ("tc.evK=frame,ms,KIND,id,slot,x,y"), oldest first."""
    out = []
    for k in sorted((int(k[5:]) for k in counters if k.startswith("tc.ev") and k[5:].isdigit())):
        f = counters[f"tc.ev{k}"].split(",")
        if len(f) == 7:
            out.append({"frame": int(f[0]), "ms": int(f[1]), "kind": f[2], "id": int(f[3]), "slot": int(f[4]),
                        "x": int(f[5]), "y": int(f[6])})
    return out


def transition_lines(counters: dict, n: int = 4) -> list[str]:
    if "tc.frames" not in counters:
        return []
    g = counters.get
    out = [f"frames {g('tc.frames')}  failed {g('tc.failed_frames')}  points {g('tc.points_in')}  "
           f"out of range {g('tc.out_of_range')}  overflow {g('overflow', '-')}  transitions {g('tc.transitions')}  "
           f"frame max {g('tc.frame_us_max')} us"]
    for t in reversed(transitions(counters)[-n:]):
        out.append(f"frame {t['frame']} ({t['ms']} ms): {t['kind']} id{t['id']} #{t['slot']} at {t['x']},{t['y']}")
    return out


def trail_update(trails: dict, contacts: list[dict], length: int) -> dict:
    """Keeps the last `length` positions per contact, keyed by (slot, track ID): a new ID in a
    slot starts a new trail; contacts no longer reported lose theirs."""
    if length <= 0:
        return {}
    live = {}
    for c in contacts:
        if c["state"] == "UP":
            continue
        key = (c["slot"], c["id"])
        pts = trails.get(key, [])
        if not pts or pts[-1] != (c["x"], c["y"]):
            pts = (pts + [(c["x"], c["y"])])[-length:]
        live[key] = pts
    return live


def header(counters: dict, rtt_ms: float) -> list[str]:
    g = counters.get
    return [
        f"contacts {g('count', '-')}   max seen {g('max_seen', '-')}   reads {g('reads', '-')}   "
        f"by count 0..5 {g('by_count', '-')}",
        f"new {g('new', '-')}  up {g('up', '-')}  moves {g('moves', '-')}  multi ms max {g('multi_ms_max', '-')}",
        f"malformed {g('malformed', '-')}  dup ids {g('duplicate_ids', '-')}  reorders {g('reorders', '-')}  "
        f"dropouts {g('dropouts', '-')}  id changes {g('id_changes', '-')}  lost {g('lost', '-')}   "
        f"poll {rtt_ms:.0f} ms",
    ]


# ---- the X11 connection ------------------------------------------------------------------

class X:
    def __init__(self, host: str, port: int, cookie: bytes | None):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        name = b"MIT-MAGIC-COOKIE-1" if cookie else b""
        data = cookie or b""
        self.s.sendall(struct.pack("<BxHHHHxx", ord("l"), 11, 0, len(name), len(data)) + pad(name) + pad(data))
        head = self.recv(8)
        if head[0] != 1:
            raise SystemExit(f"X11 setup refused: {self.recv(struct.unpack_from('<H', head, 6)[0] * 4)[:head[1]]!r}")
        body = self.recv(struct.unpack_from("<H", head, 6)[0] * 4)
        self.base, self.mask = struct.unpack_from("<II", body, 4)
        vlen, = struct.unpack_from("<H", body, 16)
        nformats = body[21]
        scr = 32 + len(pad(b"\0" * vlen)) + 8 * nformats
        self.root, = struct.unpack_from("<I", body, scr)
        self.width, self.height = struct.unpack_from("<HH", body, scr + 20)
        self.next_id = 0
        self.seq = 0
        self.events: list[bytes] = []

    def recv(self, n: int) -> bytes:
        b = b""
        while len(b) < n:
            part = self.s.recv(n - len(b))
            if not part:
                raise ConnectionError("X server closed the connection")
            b += part
        return b

    def new_id(self) -> int:
        self.next_id += 1
        return self.base | (self.next_id & self.mask)

    def send(self, data: bytes) -> int:
        self.s.sendall(data)
        self.seq = (self.seq + 1) & 0xFFFF
        return self.seq

    def reply(self, seq: int) -> bytes:
        """Reads until the reply for seq; events are kept, an error raises."""
        while True:
            head = self.recv(32)
            if head[0] == 1:
                extra = self.recv(struct.unpack_from("<I", head, 4)[0] * 4)
                if struct.unpack_from("<H", head, 2)[0] == seq:
                    return head + extra
            elif head[0] == 0:
                raise RuntimeError(f"X11 error {head[1]} for request seq {struct.unpack_from('<H', head, 2)[0]}")
            else:
                self.events.append(head)

    def intern(self, name: str) -> int:
        n = name.encode()
        r = self.reply(self.send(req(16, 0, struct.pack("<H2x", len(n)) + pad(n))))
        return struct.unpack_from("<I", r, 8)[0]

    def get_text_property(self, atom: int) -> str:
        r = self.reply(self.send(req(20, 0, struct.pack("<IIIII", self.root, atom, 0, 0, 16384 // 4))))
        n = struct.unpack_from("<I", r, 16)[0]
        return r[32:32 + n].decode(errors="replace")


class Display:
    """The full-screen diagnostics window and its drawing."""

    def __init__(self, x: X):
        self.x = x
        self.win, self.gc, self.font = x.new_id(), x.new_id(), x.new_id()
        # override-redirect: the window manager leaves it alone; Exposure only
        x.send(req(1, 0, struct.pack("<IIhhHHHHII", self.win, x.root, 0, 0, x.width, x.height, 0, 1, 0,
                                     0x2 | 0x200 | 0x800) + struct.pack("<III", BG, 1, 0x8000)))
        x.send(req(8, 0, struct.pack("<I", self.win)))
        fixed = b"fixed"
        x.send(req(45, 0, struct.pack("<IH2x", self.font, len(fixed)) + pad(fixed)))
        x.send(req(55, 0, struct.pack("<III", self.gc, self.win, 0x4 | 0x8 | 0x10 | 0x4000) +
                   struct.pack("<IIII", FG, BG, 3, self.font)))

    def colour(self, fg: int, width: int = 3) -> None:
        self.x.send(req(56, 0, struct.pack("<II", self.gc, 0x4 | 0x10) + struct.pack("<II", fg, width)))

    def text(self, x: int, y: int, s: str) -> None:
        b = s.encode("latin-1", "replace")[:255]
        self.x.send(req(76, len(b), struct.pack("<IIhh", self.win, self.gc, x, y) + pad(b)))

    def draw(self, contacts: list[dict], counters: dict, rtt_ms: float, trails: dict | None = None) -> None:
        x = self.x
        self.colour(BG)
        x.send(req(70, 0, struct.pack("<II", self.win, self.gc) + struct.pack("<hhHH", 0, 0, x.width, x.height)))
        self.colour(DIM, 1)
        for i, line in enumerate(header(counters, rtt_ms) + transition_lines(counters) + gesture_lines(counters)):
            self.text(6, 14 + 14 * i, line)
        for (slot, _), pts in (trails or {}).items():  # trails first: the markers stay on top
            if len(pts) > 1:
                self.colour(SLOT_COLOURS[slot % len(SLOT_COLOURS)], 1)
                x.send(req(65, 0, struct.pack("<II", self.win, self.gc) +
                           b"".join(struct.pack("<hh", px, py) for px, py in pts)))
        for c in contacts:
            col = SLOT_COLOURS[c["slot"] % len(SLOT_COLOURS)] if c["state"] != "UP" else DIM
            self.colour(col, 3)
            for kind, args in shape_ops(c["slot"], c["x"], c["y"]):
                if kind == "arc":
                    ax, ay, w, h = args
                    x.send(req(68, 0, struct.pack("<II", self.win, self.gc) + struct.pack("<hhHHhh", ax, ay, w, h, 0,
                                                                                          360 * 64)))
                elif kind == "rect":
                    x.send(req(67, 0, struct.pack("<II", self.win, self.gc) + struct.pack("<hhHH", *args)))
                elif kind == "lines":
                    x.send(req(65, 0, struct.pack("<II", self.win, self.gc) +
                               b"".join(struct.pack("<hh", px, py) for px, py in args)))
                else:
                    x.send(req(66, 0, struct.pack("<II", self.win, self.gc) +
                               b"".join(struct.pack("<hhhh", *seg) for seg in args)))
            self.colour(FG, 1)
            lx = min(max(4, c["x"] - R), x.width - 6 * 40)
            ly = c["y"] + R + 16 if c["y"] + R + 20 < x.height else c["y"] - R - 6
            self.text(lx, ly, label(c))
        for i, c in enumerate(contacts):  # a list at the bottom as well: readable under fingers
            self.text(6, x.height - 8 - 14 * (len(contacts) - 1 - i), label(c))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--display", required=True, help="host:N, as in DISPLAY (the panel's address)")
    ap.add_argument("--no-auth", action="store_true", help="connect without a cookie (host test server)")
    ap.add_argument("--hz", type=float, default=20.0, help="diagnostics polls per second")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0: until Ctrl-C)")
    ap.add_argument("--trail", type=int, default=24, help="positions kept per contact (0: no trails)")
    ap.add_argument("--log", help="append one JSON line per change")
    ap.add_argument("--fake", help="render the mt.* lines of this file instead of polling (tests)")
    a = ap.parse_args(argv)
    host, _, num = a.display.rpartition(":")
    x = X(host or "127.0.0.1", 6000 + int(num.split(".")[0]), None if a.no_auth else cookie_for(a.display))
    diag_atom = x.intern("_WOODLAND_DIAG") if not a.fake else 0
    d = Display(x)
    log = open(a.log, "a") if a.log else None
    t_end = time.monotonic() + a.seconds if a.seconds else None
    last_seq, redraws, trails = None, 0, {}
    try:
        while t_end is None or time.monotonic() < t_end:
            t0 = time.monotonic()
            if a.fake:
                with open(a.fake) as f:
                    text = f.read()
            else:
                text = x.get_text_property(diag_atom)
            rtt = (time.monotonic() - t0) * 1e3
            contacts, counters = parse(text)
            trails = trail_update(trails, contacts, a.trail)
            exposed = any(e[0] & 0x7F == 12 for e in x.events)
            x.events.clear()
            seq = counters.get("seq")
            if seq != last_seq or exposed or redraws == 0:
                d.draw(contacts, counters, rtt, trails)
                redraws += 1
                last_seq = seq
                if log:
                    log.write(json.dumps({"t": round(time.time(), 3), "seq": seq, "contacts": contacts,
                                          "counters": counters, "poll_ms": round(rtt, 1)}) + "\n")
                    log.flush()
            # a round trip so drawing errors surface here, then wait for the next poll
            x.reply(x.send(req(43)))
            left = 1.0 / a.hz - (time.monotonic() - t0)
            if left > 0:
                select.select([], [], [], left)
    except KeyboardInterrupt:
        pass
    finally:
        if log:
            log.close()
    print(json.dumps({"redraws": redraws, "last_seq": last_seq}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
