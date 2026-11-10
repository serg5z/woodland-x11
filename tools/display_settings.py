#!/usr/bin/env python3
"""Display Settings for the Woodland X11 panel: the LCD pixel clock.

Needs the pixel-clock test image (-DX11_PCLK). The panel reports its state in _WOODLAND_DIAG
("pclk.*"); a request is the root property _WOODLAND_PCLK_REQUEST ("request <MHz> <nonce>" or
"reset <nonce>"), which the panel handles once and applies by restarting. After the restart
the panel itself asks "Keep it?" for 10 s (no answer = revert): the X client is disconnected by
the restart, so confirming never depends on it.

    tools/display_settings.py --display PANEL_IP:0                 (window: 12/14/16, Apply, Reset)
    tools/display_settings.py --display PANEL_IP:0 --status
    tools/display_settings.py --display PANEL_IP:0 --request 14
    tools/display_settings.py --display PANEL_IP:0 --reset         (16 MHz, restart)
"""
from __future__ import annotations

import argparse
import json
import os
import random
import select
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from touch_diag import X, cookie_for, pad, req  # noqa: E402

OPTIONS = (12, 14, 16)
REQUEST_ATOM = "_WOODLAND_PCLK_REQUEST"
XA_STRING = 31
BG, FG, DIM, SEL, OK, WARN = 0x2104, 0xFFFF, 0x8410, 0x04FF, 0x07E0, 0xF800


def pclk_state(diag: str) -> dict:
    out = {}
    for line in diag.splitlines():
        k, sep, v = line.partition("=")
        if sep and (k.startswith("pclk.") or k == "lcd.fps_nominal"):
            out[k] = v
    return out


def request_text(mhz: int | None, nonce: int) -> str:
    if mhz is None:
        return f"reset {nonce}"
    if mhz not in OPTIONS:
        raise ValueError(f"{mhz} MHz is not offered ({OPTIONS})")
    return f"request {mhz} {nonce}"


def set_root_text(x: X, atom: int, text: str) -> None:
    data = text.encode()
    x.send(req(18, 0, struct.pack("<IIIB3xI", x.root, atom, XA_STRING, 8, len(data)) + pad(data)))
    x.reply(x.send(req(43)))  # a round trip: errors surface here


def intern(x: X, name: str, only_if_exists: bool) -> int:
    n = name.encode()
    r = x.reply(x.send(req(16, 1 if only_if_exists else 0, struct.pack("<H2x", len(n)) + pad(n))))
    return struct.unpack_from("<I", r, 8)[0]


def read_diag(x: X) -> str:
    atom = intern(x, "_WOODLAND_DIAG", True)
    return x.get_text_property(atom) if atom else ""


def describe(st: dict) -> str:
    if "pclk.applied_mhz" not in st:
        return "this panel image has no pixel-clock setting (not a -DX11_PCLK build)"
    s = f"running {st['pclk.applied_mhz']} MHz ({st.get('lcd.fps_nominal', '?')} fps), confirmed {st['pclk.confirmed_mhz']} MHz"
    if st.get("pclk.trial") == "1":
        s += ", TRIAL (keep it on the panel)"
    if st.get("pclk.restricted") == "1":
        s += f", stored {st.get('pclk.stored_confirmed')} MHz not used"
    return s + f"; last: {st.get('pclk.last', '-')}"


class Window:
    """Rows of big buttons: 12 / 14 / 16 MHz, then Apply and Reset to 16."""
    BTN = {12: (60, 150, 230, 240), 14: (280, 150, 450, 240), 16: (500, 150, 670, 240),
           "apply": (60, 290, 330, 370), "reset": (400, 290, 670, 370)}

    def __init__(self, x: X):
        self.x = x
        self.win, self.gc, self.font = x.new_id(), x.new_id(), x.new_id()
        x.send(req(1, 0, struct.pack("<IIhhHHHHII", self.win, x.root, 0, 0, x.width, x.height, 0, 1, 0,
                                     0x2 | 0x200 | 0x800) + struct.pack("<III", BG, 1, 0x8000 | 0x4)))
        x.send(req(8, 0, struct.pack("<I", self.win)))
        f = b"fixed"
        x.send(req(45, 0, struct.pack("<IH2x", self.font, len(f)) + pad(f)))
        x.send(req(55, 0, struct.pack("<III", self.gc, self.win, 0x4 | 0x8 | 0x4000) + struct.pack("<III", FG, BG, self.font)))

    def fg(self, c: int) -> None:
        self.x.send(req(56, 0, struct.pack("<II", self.gc, 0x4) + struct.pack("<I", c)))

    def fill(self, x0, y0, x1, y1, c) -> None:
        self.fg(c)
        self.x.send(req(70, 0, struct.pack("<II", self.win, self.gc) + struct.pack("<hhHH", x0, y0, x1 - x0, y1 - y0)))

    def text(self, x, y, s, c=FG) -> None:
        self.fg(c)
        b = s.encode("latin-1", "replace")[:255]
        self.x.send(req(74, 0, struct.pack("<IIhh", self.win, self.gc, x, y) + bytes([len(b), 0]) + b + b"\0" * (-(len(b) + 2) % 4)))

    def draw(self, st: dict, selected: int, note: str) -> None:
        self.fill(0, 0, self.x.width, self.x.height, BG)
        self.text(60, 40, "Display Settings: LCD pixel clock", FG)
        self.text(60, 70, describe(st), DIM)
        self.text(60, 100, "Choose, then Apply: the panel restarts and asks 'Keep it?' for 10 s.", DIM)
        running = st.get("pclk.applied_mhz")
        for mhz in OPTIONS:
            x0, y0, x1, y1 = self.BTN[mhz]
            self.fill(x0, y0, x1, y1, SEL if mhz == selected else 0x4208)
            mark = "  (running)" if str(mhz) == running else ""
            self.text(x0 + 20, y0 + 50, f"{mhz} MHz{mark}", FG)
        for key, label, c in (("apply", f"Apply {selected} MHz", OK), ("reset", "Reset to 16 MHz", WARN)):
            x0, y0, x1, y1 = self.BTN[key]
            self.fill(x0, y0, x1, y1, c)
            self.text(x0 + 20, y0 + 45, label, 0x0000 if key == "apply" else FG)
        self.text(60, 420, note, FG)

    def hit(self, px: int, py: int):
        for key, (x0, y0, x1, y1) in self.BTN.items():
            if x0 <= px < x1 and y0 <= py < y1:
                return key
        return None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--display", required=True, help="host:N, as in DISPLAY (the panel's address)")
    ap.add_argument("--no-auth", action="store_true")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--status", action="store_true")
    g.add_argument("--request", type=int, choices=OPTIONS)
    g.add_argument("--reset", action="store_true")
    ap.add_argument("--seconds", type=float, default=0, help="window mode: close after this long")
    a = ap.parse_args(argv)
    host, _, num = a.display.rpartition(":")
    x = X(host or "127.0.0.1", 6000 + int(num.split(".")[0]), None if a.no_auth else cookie_for(a.display))
    if a.status or a.request or a.reset:
        st = pclk_state(read_diag(x))
        if a.status:
            print(json.dumps({"state": st, "summary": describe(st)}))
            return 0
        text = request_text(None if a.reset else a.request, random.randrange(1, 2**31))
        set_root_text(x, intern(x, REQUEST_ATOM, False), text)
        print(json.dumps({"sent": text, "before": describe(st)}))
        return 0
    w = Window(x)
    atom = intern(x, REQUEST_ATOM, False)
    st = pclk_state(read_diag(x))
    selected = int(st.get("pclk.applied_mhz", 16)) if st.get("pclk.applied_mhz", "").isdigit() else 16
    note, t_end, last = "", time.monotonic() + a.seconds if a.seconds else None, 0.0
    w.draw(st, selected, note)
    try:
        while t_end is None or time.monotonic() < t_end:
            r, _, _ = select.select([x.s], [], [], 0.5)
            events = [x.recv(32)] if r else []
            if time.monotonic() - last > 2:
                last = time.monotonic()
                new = pclk_state(read_diag(x))
                if new != st:  # repaint only on a change: no periodic flicker
                    st = new
                    events.append(None)
            events += x.events  # events that arrived while waiting for a reply
            x.events.clear()
            redraw = False
            for ev in events:
                if ev is None or ev[0] & 0x7F == 12:  # new state, Expose
                    redraw = True
                elif ev[0] & 0x7F == 4:  # ButtonPress
                    key = w.hit(*struct.unpack_from("<hh", ev, 24))
                    if key in OPTIONS:
                        selected, note, redraw = key, "", True
                    elif key in ("apply", "reset"):
                        if key == "apply" and str(selected) == st.get("pclk.applied_mhz") and st.get("pclk.trial") != "1":
                            note = f"{selected} MHz is already running."
                        else:
                            text = request_text(None if key == "reset" else selected, random.randrange(1, 2**31))
                            set_root_text(x, atom, text)
                            note = f"Sent '{text}': the panel restarts now; confirm on the panel."
                        redraw = True
            if redraw:
                w.draw(st, selected, note)
    except (KeyboardInterrupt, ConnectionError):
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
