#!/usr/bin/env python3
"""Differential pointer-event check: the same windows, selections, grabs and pointer actions on
a reference X server (Xvfb, input injected with XTEST) and on the Woodland host server (input
through its --input "pointer X Y BUTTONS" commands). Every event each client receives is decoded
with all its fields; windows are named, so the two streams can be compared event by event. The
first divergence of each scenario is reported.

    input_diff.py --ref-port 6020 --woodland BIN [--scenario NAME] [--json OUT]

The reference server must be fresh (pointer at its startup position) and have XTEST; the
Woodland server is started by this script, once per scenario. Timestamps are compared as
properties (monotonic, equal within one action) rather than values."""
import argparse
import json
import os
import select
import socket
import struct
import subprocess
import sys
import time

EVENT_NAMES = {4: "ButtonPress", 5: "ButtonRelease", 6: "MotionNotify", 7: "EnterNotify", 8: "LeaveNotify",
               9: "FocusIn", 10: "FocusOut", 12: "Expose", 17: "DestroyNotify", 18: "UnmapNotify", 19: "MapNotify",
               21: "ReparentNotify", 22: "ConfigureNotify", 34: "MappingNotify"}
DETAIL = {0: "Ancestor", 1: "Virtual", 2: "Inferior", 3: "Nonlinear", 4: "NonlinearVirtual", 5: "Pointer",
          6: "PointerRoot", 7: "None"}
MODE = {0: "Normal", 1: "Grab", 2: "Ungrab", 3: "WhileGrabbed"}
KEYPRESS, BPRESS, BRELEASE, MOTION, ENTER, LEAVE = 2, 4, 5, 6, 7, 8
M = {"press": 1 << 2, "release": 1 << 3, "enter": 1 << 4, "leave": 1 << 5, "motion": 1 << 6, "hint": 1 << 7,
     "b1motion": 1 << 8, "bmotion": 1 << 13, "owner": 1 << 24, "focus": 1 << 21, "structure": 1 << 17,
     "substructure": 1 << 19, "redirect": 1 << 20}


class Client:
    def __init__(self, port, name):
        self.name = name
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.s.sendall(b"l\0\x0b\0\0\0\0\0\0\0\0\0")
        head = self._recv(8)
        assert head[0] == 1, "setup failed"
        body = self._recv(struct.unpack("<H", head[6:8])[0] * 4)
        self.base = struct.unpack_from("<I", body, 4)[0]
        vlen = struct.unpack_from("<H", body, 16)[0]
        self.root = struct.unpack_from("<I", body, 32 + ((vlen + 3) & ~3) + 8 * body[21])[0]
        self.next = 0
        self.seq = 0
        self.buf = b""
        self.events = []      # (arrival index, raw 32 bytes)
        self.replies = {}
        self.errors = []

    def _recv(self, n):
        b = b""
        while len(b) < n:
            part = self.s.recv(n - len(b))
            if not part:
                raise ConnectionError("closed")
            b += part
        return b

    def new_id(self):
        self.next += 1
        return self.base + self.next

    def req(self, op, d1=0, body=b""):
        body += b"\0" * (-len(body) % 4)
        self.s.sendall(struct.pack("<BBH", op, d1, 1 + len(body) // 4) + body)
        self.seq = (self.seq + 1) & 0xFFFF
        return self.seq

    def pump(self):
        """Reads what has arrived."""
        self.s.setblocking(False)
        try:
            while True:
                part = self.s.recv(65536)
                if not part:
                    break
                self.buf += part
        except (BlockingIOError, socket.timeout):
            pass
        finally:
            self.s.setblocking(True)
            self.s.settimeout(10)
        while len(self.buf) >= 32:
            m = self.buf[:32]
            if m[0] == 1:
                n = 32 + 4 * struct.unpack_from("<I", m, 4)[0]
                if len(self.buf) < n:
                    break
                self.replies[struct.unpack_from("<H", m, 2)[0]] = self.buf[:n]
                self.buf = self.buf[n:]
            else:
                self.buf = self.buf[32:]
                if m[0] == 0:
                    self.errors.append((m[1], struct.unpack_from("<I", m, 4)[0], m[10], struct.unpack_from("<H", m, 2)[0]))
                else:
                    self.events.append(m)

    def roundtrip(self, op, d1=0, body=b""):
        seq = self.req(op, d1, body)
        end = time.time() + 10
        while seq not in self.replies:
            r, _, _ = select.select([self.s], [], [], max(0, end - time.time()))
            if not r:
                raise TimeoutError(f"{self.name}: no reply to {op}")
            self.pump()
            if any(e[3] == seq for e in self.errors):
                return None
        return self.replies.pop(seq)

    def sync(self):
        return self.roundtrip(43)

    def window(self, parent, x, y, w, h, mask=0, dnp=0, override=0, bw=0):
        wid = self.new_id()
        vmask, vals = 0, b""
        if override:
            vmask |= 1 << 9
            vals += struct.pack("<I", 1)
        if mask:
            vmask |= 1 << 11
            vals += struct.pack("<I", mask)
        if dnp:
            vmask |= 1 << 12
            vals += struct.pack("<I", dnp)
        self.req(1, 0, struct.pack("<IIhhHHHHII", wid, parent, x, y, w, h, bw, 1, 0, vmask) + vals)
        return wid

    def select(self, wid, mask):
        self.req(2, 0, struct.pack("<III", wid, 1 << 11, mask))

    def map(self, wid):
        self.req(8, 0, struct.pack("<I", wid))

    def unmap(self, wid):
        self.req(10, 0, struct.pack("<I", wid))

    def destroy(self, wid):
        self.req(4, 0, struct.pack("<I", wid))

    def reparent(self, wid, parent, x, y):
        self.req(7, 0, struct.pack("<IIhh", wid, parent, x, y))

    def raise_(self, wid):
        self.req(12, 0, struct.pack("<IHxxI", wid, 1 << 6, 0))

    def move(self, wid, x, y):
        self.req(12, 0, struct.pack("<IHxxii", wid, 3, x, y))

    def grab_button(self, wid, mask, sync, button=1, owner=0, mods=0x8000, confine=0):
        self.req(28, owner, struct.pack("<IHBBIIBxH", wid, mask, 0 if sync else 1, 1, confine, 0, button, mods))

    def grab_pointer(self, wid, mask, sync=False, owner=0, confine=0):
        r = self.roundtrip(26, owner, struct.pack("<IHBBIII", wid, mask, 0 if sync else 1, 1, confine, 0, 0))
        return r[1] if r else None

    def ungrab_pointer(self):
        self.req(27, 0, struct.pack("<I", 0))

    def allow(self, mode):
        self.req(35, mode, struct.pack("<I", 0))

    def focus(self, wid, revert=1):
        self.req(42, revert, struct.pack("<II", wid, 0))

    def grab_keyboard(self, wid):
        r = self.roundtrip(31, 0, struct.pack("<IIBBxx", wid, 0, 1, 1))
        return r[1] if r else None

    def ungrab_keyboard(self):
        self.req(32, 0, struct.pack("<I", 0))

    def query_pointer(self):
        r = self.roundtrip(38, 0, struct.pack("<I", self.root))
        root, child, rx, ry, wx, wy, mask = struct.unpack_from("<IIhhhhH", r, 8)
        return {"child": child, "x": rx, "y": ry, "mask": mask}


class XvfbInput:
    """Pointer input through XTEST FakeInput on a fresh connection."""

    def __init__(self, port):
        self.c = Client(port, "xtest")
        r = self.c.roundtrip(98, 0, struct.pack("<HH", 5, 0) + b"XTEST")
        assert r[8], "no XTEST"
        self.op = r[9]
        self.buttons = 0
        self.x = self.y = None

    def fake(self, type_, detail, x=0, y=0):
        self.c.req(self.op, 2, struct.pack("<BBxxIIxxxxxxxxhhxxxxxxxB", type_, detail, 0, self.c.root if type_ == MOTION else 0,
                                           x, y, 0))

    def pointer(self, x, y, buttons):
        if (x, y) != (self.x, self.y):
            self.fake(MOTION, 0, x, y)
            self.x, self.y = x, y
        for b in range(1, 6):
            was, now = self.buttons >> (b - 1) & 1, buttons >> (b - 1) & 1
            if was != now:
                self.fake(BPRESS if now else BRELEASE, b)
        self.buttons = buttons
        self.c.sync()

    def close(self):
        self.c.s.close()


class WoodlandInput:
    def __init__(self, proc):
        self.p = proc

    def pointer(self, x, y, buttons):
        self.p.stdin.write(f"pointer {x} {y} {buttons}\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        assert line.startswith("ack"), line

    def close(self):
        pass


def decode(e, names):
    t = e[0] & 0x7F
    n = lambda w: names.get(w, "0" if w == 0 else hex(w))
    out = {"type": EVENT_NAMES.get(t, str(t)), "sent": bool(e[0] & 0x80)}
    if t in (BPRESS, BRELEASE, MOTION, ENTER, LEAVE):
        detail = e[1]
        time_, root, ev, child, rx, ry, ex, ey, state = struct.unpack_from("<IIIIhhhhH", e, 4)
        out.update(detail=DETAIL.get(detail, detail) if t in (ENTER, LEAVE) else detail, time=time_,
                   root=n(root), window=n(ev), child=n(child), root_xy=[rx, ry], event_xy=[ex, ey], state=hex(state))
        if t in (ENTER, LEAVE):
            out.update(mode=MODE.get(e[30], e[30]), focus=bool(e[31] & 1), same_screen=bool(e[31] & 2))
        else:
            out.update(same_screen=bool(e[30]))
    elif t in (9, 10):
        out.update(detail=DETAIL.get(e[1], e[1]), window=n(struct.unpack_from("<I", e, 4)[0]), mode=MODE.get(e[8], e[8]))
    elif t == 12:
        return None  # exposures: not compared (rendering, not input)
    else:
        out.update(detail=e[1], window=n(struct.unpack_from("<I", e, 4)[0]))
    out["seq"] = struct.unpack_from("<H", e, 2)[0]
    return out


# ---- scenarios: functions(ctx) using ctx.client(name), ctx.win(...), ctx.ptr(x, y, buttons) ----

def sc_click(ctx):
    a = ctx.client("app")
    w = ctx.win(a, "W", a.root, 100, 100, 200, 150, M["press"] | M["release"] | M["motion"] | M["enter"] | M["leave"])
    ctx.settle()
    ctx.ptr(150, 150, 0)
    ctx.ptr(150, 150, 1)
    ctx.ptr(150, 150, 0)


def sc_double_and_rapid(ctx):
    a = ctx.client("app")
    ctx.win(a, "W", a.root, 100, 100, 200, 150, M["press"] | M["release"])
    ctx.settle()
    ctx.ptr(150, 150, 0)
    for _ in range(2):
        ctx.ptr(150, 150, 1), ctx.ptr(150, 150, 0)
    for _ in range(8):  # rapid, no settling between
        ctx.ptr(150, 150, 1, sync=False), ctx.ptr(150, 150, 0, sync=False)
    ctx.ptr(151, 150, 0)


def sc_drag(ctx):
    a = ctx.client("app")
    ctx.win(a, "W", a.root, 100, 100, 300, 200, M["press"] | M["release"] | M["bmotion"])
    ctx.win(a, "M1", a.root, 450, 100, 100, 100, M["b1motion"])
    ctx.settle()
    ctx.ptr(120, 120, 0)
    ctx.ptr(120, 120, 1)
    for x in (140, 180, 250, 380, 470, 600):  # out of W, over M1, off both, still held
        ctx.ptr(x, 130, 1)
    ctx.ptr(600, 130, 0)
    ctx.ptr(470, 130, 1)
    ctx.ptr(480, 140, 1)
    ctx.ptr(480, 140, 0)


def sc_crossing(ctx):
    a = ctx.client("app")
    m = M["enter"] | M["leave"]
    p = ctx.win(a, "P", a.root, 100, 100, 400, 250, m, bw=2)
    ctx.win(a, "A", p, 20, 20, 150, 150, m)
    b = ctx.win(a, "B", p, 220, 20, 150, 150, m)
    ctx.win(a, "B1", b, 10, 10, 50, 50, m)
    ctx.settle()
    for x, y in ((50, 50), (150, 150), (200, 200), (350, 150), (335, 135), (450, 300), (102, 300), (50, 50)):
        ctx.ptr(x, y, 0)


def sc_crossing_grabbed(ctx):
    a = ctx.client("app")
    m = M["enter"] | M["leave"] | M["press"] | M["release"]
    p = ctx.win(a, "P", a.root, 100, 100, 400, 250, M["enter"] | M["leave"])
    ctx.win(a, "A", p, 20, 20, 150, 150, m)
    ctx.win(a, "B", p, 220, 20, 150, 150, m)
    ctx.settle()
    ctx.ptr(150, 150, 0)
    ctx.ptr(150, 150, 1)   # automatic grab on A
    ctx.ptr(350, 150, 1)   # into B while grabbed
    ctx.ptr(350, 150, 0)   # release over B: Ungrab crossings
    ctx.ptr(350, 160, 0)


def sc_propagation(ctx):
    a, other = ctx.client("app"), ctx.client("other")
    p = ctx.win(a, "P", a.root, 100, 100, 400, 250, M["press"] | M["release"] | M["enter"] | M["leave"])
    c = ctx.win(a, "C", p, 50, 50, 100, 100, M["enter"] | M["leave"])            # does not select presses
    d = ctx.win(a, "D", p, 200, 50, 100, 100, M["enter"] | M["leave"], dnp=M["press"])  # do-not-propagate
    other.select(c, M["release"])
    ctx.settle()
    ctx.ptr(200, 200, 0)
    ctx.ptr(200, 200, 1)   # in C: to P, automatic grab on P (crossings?)
    ctx.ptr(205, 205, 1)
    ctx.ptr(205, 205, 0)
    ctx.ptr(350, 200, 0)
    ctx.ptr(350, 200, 1)   # in D: do-not-propagate press: nobody
    ctx.ptr(350, 200, 0)


def sc_wm_passive_sync(ctx, wm_mask=None, name=""):
    wm, app = ctx.client("wm"), ctx.client("app")
    frame = ctx.win(wm, "frame", wm.root, 100, 100, 300, 230, M["substructure"] | M["enter"] | M["leave"])
    win = ctx.win(app, "appwin", app.root, 0, 0, 300, 200, M["press"] | M["release"] | M["enter"] | M["leave"] | M["bmotion"])
    app.map(win)
    app.sync()
    wm.reparent(win, frame, 0, 30)
    wm.sync()
    wm.grab_button(frame, wm_mask if wm_mask is not None else M["release"], True)
    ctx.settle()
    ctx.ptr(150, 200, 0)
    for _ in range(2):
        ctx.ptr(150, 200, 1)
        wm.pump()
        ctx.note("allow ReplayPointer")
        wm.allow(2)
        wm.sync()
        ctx.ptr(160, 205, 1)
        ctx.ptr(160, 205, 0)


def sc_wm_blackbox(ctx):
    sc_wm_passive_sync(ctx, M["press"] | M["release"] | M["bmotion"])


def sc_active_grab(ctx):
    a, g = ctx.client("app"), ctx.client("grabber")
    w = ctx.win(a, "W", a.root, 100, 100, 200, 150, M["press"] | M["release"] | M["enter"] | M["leave"] | M["motion"])
    gw = ctx.win(g, "G", g.root, 400, 100, 200, 150, M["enter"] | M["leave"])
    ctx.settle()
    ctx.ptr(150, 150, 0)
    ctx.note("grab_pointer G owner=0 mask=press|release|motion -> %s" % g.grab_pointer(gw, M["press"] | M["release"] | M["motion"]))
    g.sync()
    ctx.ptr(160, 150, 0)
    ctx.ptr(160, 150, 1)
    ctx.ptr(160, 150, 0)
    g.ungrab_pointer()
    g.sync()
    ctx.ptr(170, 150, 0)
    ctx.note("grab_pointer G owner=1 -> %s" % g.grab_pointer(gw, M["press"], owner=1))
    g.sync()
    ctx.ptr(500, 150, 1)
    ctx.ptr(500, 150, 0)
    g.ungrab_pointer()
    g.sync()
    ctx.ptr(510, 150, 0)


def sc_destroy_during_grab(ctx):
    a, o = ctx.client("app"), ctx.client("observer")
    p = ctx.win(o, "P", o.root, 50, 50, 500, 300, M["press"] | M["release"] | M["enter"] | M["leave"] | M["bmotion"])
    w = ctx.win(a, "W", p, 50, 50, 200, 150, M["press"] | M["release"] | M["bmotion"] | M["enter"] | M["leave"])
    ctx.settle()
    ctx.ptr(150, 150, 0)
    ctx.ptr(150, 150, 1)     # automatic grab on W
    ctx.note("app destroys W while the button is held")
    a.destroy(w)
    a.sync()
    ctx.ptr(170, 160, 1)
    ctx.ptr(170, 160, 0)
    ctx.ptr(180, 160, 1)
    ctx.ptr(180, 160, 0)


def sc_close_during_grab(ctx):
    a, o = ctx.client("app"), ctx.client("observer")
    p = ctx.win(o, "P", o.root, 50, 50, 500, 300, M["press"] | M["release"] | M["enter"] | M["leave"] | M["bmotion"])
    w = ctx.win(a, "W", p, 50, 50, 200, 150, M["press"] | M["release"] | M["bmotion"])
    ctx.settle()
    ctx.ptr(150, 150, 0)
    ctx.ptr(150, 150, 1)     # automatic grab on W (client app)
    ctx.note("app closes its connection while the button is held")
    a.s.close()
    del ctx.clients["app"]
    time.sleep(0.3)
    ctx.ptr(170, 160, 1)
    ctx.ptr(170, 160, 0)
    ctx.ptr(180, 160, 1)
    ctx.ptr(180, 160, 0)


def sc_unmap_active_grab(ctx):
    a = ctx.client("app")
    w = ctx.win(a, "W", a.root, 100, 100, 200, 150, M["press"] | M["release"] | M["enter"] | M["leave"])
    o = ctx.win(a, "O", a.root, 400, 100, 200, 150, M["press"] | M["release"] | M["enter"] | M["leave"])
    ctx.settle()
    ctx.ptr(450, 150, 0)
    ctx.note("grab W -> %s" % a.grab_pointer(w, M["press"] | M["release"] | M["enter"] | M["leave"]))
    ctx.note("unmap W")
    a.unmap(w)
    a.sync()
    ctx.ptr(460, 150, 1)
    ctx.ptr(460, 150, 0)


def sc_focus(ctx):
    a = ctx.client("app")
    fm = M["focus"] | M["enter"] | M["leave"]
    w = ctx.win(a, "W", a.root, 100, 100, 200, 150, fm)
    c = ctx.win(a, "C", w, 20, 20, 50, 50, fm)
    o = ctx.win(a, "O", a.root, 400, 100, 200, 150, fm)
    a.select(a.root, M["focus"])
    ctx.names[a.root] = "root"
    ctx.settle()
    ctx.ptr(130, 130, 0)
    for target, label in ((w, "W"), (c, "C"), (o, "O"), (1, "PointerRoot"), (0, "None"), (w, "W")):
        ctx.note("SetInputFocus " + label)
        a.focus(target)
        a.sync()
        ctx.settle(0.05)
    ctx.note("GrabKeyboard O -> %s" % a.grab_keyboard(o))
    a.sync()
    ctx.note("UngrabKeyboard")
    a.ungrab_keyboard()
    a.sync()
    ctx.settle(0.05)


def sc_hint(ctx):
    a = ctx.client("app")
    w = ctx.win(a, "W", a.root, 100, 100, 300, 200, M["motion"] | M["hint"])
    ctx.settle()
    for x in (150, 160, 170):
        ctx.ptr(x, 150, 0)
    ctx.note("QueryPointer")
    a.query_pointer()
    for x in (180, 190):
        ctx.ptr(x, 150, 0)


SCENARIOS = {f.__name__[3:]: f for f in (sc_click, sc_double_and_rapid, sc_drag, sc_crossing, sc_crossing_grabbed,
                                          sc_propagation, sc_wm_passive_sync, sc_wm_blackbox, sc_active_grab,
                                          sc_destroy_during_grab, sc_close_during_grab, sc_unmap_active_grab, sc_focus, sc_hint)}


class Ctx:
    def __init__(self, port, inp):
        self.port, self.inp = port, inp
        self.clients = {}
        self.names = {}
        self.log = []  # ("action", text) markers interleaved by the collector

    def client(self, name):
        c = Client(self.port, name)
        self.clients[name] = c
        self.names[c.root] = "root"
        return c

    def win(self, c, name, parent, x, y, w, h, mask, dnp=0, bw=0):
        wid = c.window(parent, x, y, w, h, mask, dnp, bw=bw)
        self.names[wid] = name
        c.map(wid)
        c.sync()  # other clients may use it next: it must exist before their requests
        return wid

    def collect(self):
        for c in self.clients.values():
            c.sync()
        for name, c in self.clients.items():
            for e in c.events:
                d = decode(e, self.names)
                if d:
                    self.log.append((name, d))
            c.events = []

    def settle(self, t=0.15):
        time.sleep(t)
        self.collect()
        self.log.append(("--", {"mark": "settled"}))

    def note(self, text):
        self.collect()
        self.log.append(("--", {"mark": text}))

    def ptr(self, x, y, b, sync=True):
        self.inp.pointer(x, y, b)
        if sync:
            self.collect()
            self.log.append(("--", {"mark": f"pointer {x} {y} {b}"}))


def run(scenario, port, inp_factory, preamble):
    inp = inp_factory()
    ctx = Ctx(port, inp)
    preamble(inp)
    SCENARIOS[scenario](ctx)
    ctx.settle(0.1)
    for c in ctx.clients.values():
        ctx.log += [(c.name, {"error": list(e)}) for e in c.errors]
        c.s.close()
    inp.close()
    return ctx.log


def comparable(entry):
    who, d = entry
    return who, {k: v for k, v in d.items() if k not in ("time", "seq")}


def first_divergence(a, b):
    for i in range(max(len(a), len(b))):
        x = comparable(a[i]) if i < len(a) else None
        y = comparable(b[i]) if i < len(b) else None
        if x != y:
            return i
    return None


def time_checks(log):
    out = []
    last = {}
    for who, d in log:
        if "time" in d:
            if who in last and d["time"] < last[who]:
                out.append(f"{who}: time went back {last[who]} -> {d['time']} at {d['type']}")
            last[who] = d["time"]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref-port", type=int, default=6020)
    ap.add_argument("--ref-cmd", default="Xvfb :{d} -screen 0 800x480x16 -nolisten unix -listen tcp -ac")
    ap.add_argument("--woodland", required=True)
    ap.add_argument("--wl-display", type=int, default=21)
    ap.add_argument("--scenario", action="append")
    ap.add_argument("--json")
    a = ap.parse_args()
    results = {}
    for name in a.scenario or list(SCENARIOS):
        # fresh servers per scenario (pointer position, focus, grabs)
        ref = subprocess.Popen(a.ref_cmd.format(d=a.ref_port - 6000).split(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wl = subprocess.Popen([a.woodland, "-d", str(a.wl_display), "--input", "-q"], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        time.sleep(1.2)
        try:
            # both pointers start at (5, 5) before any window exists
            ref_log = run(name, a.ref_port, lambda: XvfbInput(a.ref_port), lambda i: i.pointer(5, 5, 0))
            wl_log = run(name, 6000 + a.wl_display, lambda: WoodlandInput(wl), lambda i: i.pointer(5, 5, 0))
        finally:
            ref.terminate()
            wl.stdin.close()
            wl.terminate()
            ref.wait(5)
            wl.wait(5)
        i = first_divergence(ref_log, wl_log)
        results[name] = {"ref": ref_log, "woodland": wl_log, "first_divergence": i,
                         "time_ref": time_checks(ref_log), "time_woodland": time_checks(wl_log)}
        print(f"== {name}: {'identical' if i is None else 'diverges at %d' % i} "
              f"({len(ref_log)} / {len(wl_log)} entries)")
        if i is not None:
            for k in range(max(0, i - 2), min(max(len(ref_log), len(wl_log)), i + 4)):
                r = ref_log[k] if k < len(ref_log) else None
                w = wl_log[k] if k < len(wl_log) else None
                mark = "!!" if k == i else "  "
                print(f"{mark} ref: {comparable(r) if r else '-'}")
                print(f"{mark}  wl: {comparable(w) if w else '-'}")
        for t in results[name]["time_woodland"]:
            print("   woodland time:", t)
    if a.json:
        json.dump(results, open(a.json, "w"), indent=1, default=str)


if __name__ == "__main__":
    main()
