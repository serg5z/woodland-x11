#!/usr/bin/env python3
"""Differential check of the keyboard-map, modifier-map, host-access and close-down requests:
the same valid and malformed requests, in both byte orders, on a reference server (Xvfb) and on
the Woodland host server. Prints each outcome (reply fields, "ok", or the error code, bad value
and opcodes, plus any MappingNotify) side by side, then the differences.

    core_diff.py REF_PORT TEST_PORT [--reset]

--reset: both servers reset when their last client closes in Destroy mode (Xvfb always does;
Woodland with --reset), so the server-reset scenario is checked too.

State changes are undone where the protocol allows it (keyboard map, modifier map, access
control on the reference). The reference must be disposable: it is reconfigured."""
import socket
import struct
import sys
import time

ERR = {1: "Request", 2: "Value", 3: "Window", 4: "Pixmap", 5: "Atom", 6: "Cursor", 7: "Font", 8: "Match",
       9: "Drawable", 10: "Access", 11: "Alloc", 12: "Colormap", 13: "GContext", 14: "IDChoice", 15: "Name",
       16: "Length", 17: "Implementation"}


class X:
    """A raw X connection in either byte order."""

    def __init__(self, port, msb=False):
        self.e = ">" if msb else "<"
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.s.sendall((b"B\0" if msb else b"l\0") + struct.pack(self.e + "HHHH", 11, 0, 0, 0) + b"\0\0")
        head = self._recv(8)
        assert head[0] == 1, "setup failed"
        body = self._recv(struct.unpack(self.e + "H", head[6:8])[0] * 4)
        self.base = struct.unpack_from(self.e + "I", body, 4)[0]
        vlen = struct.unpack_from(self.e + "H", body, 16)[0]
        self.root = struct.unpack_from(self.e + "I", body, 32 + ((vlen + 3) & ~3) + 8 * body[21])[0]
        self.next = 0
        self.seq = 0
        self.events = []
        self.pending = {}

    def _recv(self, n):
        b = b""
        while len(b) < n:
            part = self.s.recv(n - len(b))
            if not part:
                raise ConnectionError("closed")
            b += part
        return b

    def close(self):
        self.s.close()

    def raw(self, op, d1, body=b"", units=None, reply=False):
        """Sends a request (body padded; `units` overrides the length field) and returns its
        outcome: ("reply", bytes), ("ok",) or ("error", code, bad value, minor, major)."""
        body += b"\0" * (-len(body) % 4)
        n = 1 + len(body) // 4 if units is None else units
        if units is not None:  # exactly the bytes the length field says (truncated or padded)
            body = (body + b"\0" * (4 * units))[:max(0, 4 * units - 4)]
        self.s.sendall(struct.pack(self.e + "BBH", op, d1, n) + body)
        self.seq = (self.seq + 1) & 0xFFFF
        seq = self.seq
        if not reply:  # GetInputFocus as a barrier
            self.s.sendall(struct.pack(self.e + "BBH", 43, 0, 1))
            self.seq = (self.seq + 1) & 0xFFFF
        while True:
            m = self._recv(32)
            mseq = struct.unpack_from(self.e + "H", m, 2)[0]
            if m[0] == 0:
                err = ("error", m[1], struct.unpack_from(self.e + "I", m, 4)[0],
                       struct.unpack_from(self.e + "H", m, 8)[0], m[10])
                if mseq == seq:
                    if not reply:
                        self._reply()  # the barrier's reply
                    return err
                continue
            if m[0] != 1:
                self.events.append(m)
                continue
            m += self._recv(struct.unpack_from(self.e + "I", m, 4)[0] * 4)
            if mseq == seq:
                return ("reply", m)
            if not reply and mseq == (seq + 1) & 0xFFFF:
                return ("ok",)

    def _reply(self):
        while True:
            m = self._recv(32)
            if m[0] == 1:
                return m + self._recv(struct.unpack_from(self.e + "I", m, 4)[0] * 4)
            if m[0] != 0:
                self.events.append(m)

    def sync(self):
        return self.raw(43, 0, reply=True)

    def u8(self, *v):
        return bytes(v)

    def p(self, fmt, *v):
        return struct.pack(self.e + fmt, *v)

    def new_id(self):
        self.next += 1
        return self.base + self.next

    def mapping_events(self):
        self.sync()
        out = [(m[4], m[5], m[6]) for m in self.events if m[0] & 0x7F == 34]
        self.events = []
        return out


def fmt(o):
    if o[0] == "error":  # X.Org leaves a stale bad value in Length errors: not compared
        return "Bad%s" % ERR.get(o[1], o[1]) if o[1] == 16 else "Bad%s value=%#x" % (ERR.get(o[1], o[1]), o[2])
    return o[0]


def keyboard(x, log):
    e = x.e
    gk = lambda first, n: x.raw(101, 0, x.p("BBH", first, n, 0), reply=True)
    r = gk(200, 3)
    kspk0 = r[1][1]
    saved = r[1][32:32 + 4 * kspk0 * 3]
    log("GetKeyboardMapping 200 x3: keysyms-per-keycode", kspk0)
    syms = x.p("IIII", 0x61, 0x41, 0x62, 0x42)
    log("ChangeKeyboardMapping valid (200, 2 keycodes x 2)", fmt(x.raw(100, 2, x.p("BBH", 200, 2, 0) + syms)))
    log("  MappingNotify", x.mapping_events())
    r = gk(200, 2)
    w = r[1][1]
    got = struct.unpack_from(e + "%dI" % (2 * w), r[1], 32)
    log("  readback keysyms-per-keycode", w, "keycode 200", [hex(v) for v in got[:w]], "keycode 201",
        [hex(v) for v in got[w:]])
    log("ChangeKeyboardMapping 0 keycodes", fmt(x.raw(100, 0, x.p("BBH", 8, 1, 0))))
    log("  MappingNotify", x.mapping_events())
    log("ChangeKeyboardMapping first 7", fmt(x.raw(100, 1, x.p("BBH", 7, 1, 0) + x.p("I", 0x61))))
    log("ChangeKeyboardMapping last 256", fmt(x.raw(100, 2, x.p("BBH", 255, 1, 0) + x.p("II", 0x61, 0x62))))
    log("ChangeKeyboardMapping keysyms-per-keycode 0", fmt(x.raw(100, 1, x.p("BBH", 200, 0, 0))))
    log("ChangeKeyboardMapping 3 keysyms for 2x2", fmt(x.raw(100, 2, x.p("BBH", 200, 2, 0) + x.p("III", 1, 2, 3))))
    log("ChangeKeyboardMapping length and first 7", fmt(x.raw(100, 1, x.p("BBH", 7, 1, 0) + x.p("II", 1, 2))))
    log("ChangeKeyboardMapping length 1", fmt(x.raw(100, 1, b"", units=1)))
    log("ChangeKeyboardMapping keycode 255", fmt(x.raw(100, 1, x.p("BBH", 255, 1, 0) + x.p("I", 0x63))))
    log("  MappingNotify", x.mapping_events())
    # undo: the saved keysyms at the saved width
    x.raw(100, 3, x.p("BBH", 200, kspk0, 0) + saved)
    x.raw(100, 1, x.p("BBH", 255, 1, 0) + x.p("I", 0))
    x.mapping_events()


def modifiers(x, log):
    r = x.raw(119, 0, reply=True)
    kpm = r[1][1]
    saved = r[1][32:32 + 8 * kpm]
    log("GetModifierMapping keycodes-per-modifier", kpm)

    def set_(kpm, codes, units=None):
        return x.raw(118, kpm, bytes(codes), units=units, reply=True)

    def show(o):
        return "reply status=%d" % o[1][1] if o[0] == "reply" else fmt(o)

    codes = [0] * 16
    codes[0] = 200  # Shift: 200
    codes[2 * 2] = 201  # Control: 201
    log("SetModifierMapping valid (2 per modifier)", show(set_(2, codes)))
    log("  MappingNotify", x.mapping_events())
    r = x.raw(119, 0, reply=True)
    k = r[1][1]
    log("  readback keycodes-per-modifier", k, list(r[1][32:32 + 8 * k]))
    codes = [0] * 8
    codes[0] = codes[1] = 202  # the same key in Shift and Lock
    log("SetModifierMapping a key in two modifiers", show(set_(1, codes)))
    log("  MappingNotify", x.mapping_events())
    r = x.raw(119, 0, reply=True)
    k = r[1][1]
    log("  readback keycodes-per-modifier", k, list(r[1][32:32 + 8 * k]))
    codes = [0] * 16
    codes[0] = codes[1] = 203  # twice in Shift
    log("SetModifierMapping a key twice in one modifier", show(set_(2, codes)))
    x.mapping_events()
    r = x.raw(119, 0, reply=True)
    k = r[1][1]
    log("  readback keycodes-per-modifier", k, list(r[1][32:32 + 8 * k]))
    log("SetModifierMapping 0 per modifier", show(set_(0, [])))
    log("  MappingNotify", x.mapping_events())
    r = x.raw(119, 0, reply=True)
    log("  readback keycodes-per-modifier", r[1][1], list(r[1][32:32 + 8 * r[1][1]]))
    log("SetModifierMapping keycode 7", show(set_(1, [7, 0, 0, 0, 0, 0, 0, 0])))
    log("SetModifierMapping keycode 7 after a valid one", show(set_(1, [200, 7, 0, 0, 0, 0, 0, 0])))
    log("  MappingNotify", x.mapping_events())
    log("SetModifierMapping 1 per modifier, 4 keycodes", show(set_(1, [200, 0, 0, 0])))
    log("SetModifierMapping 1 per modifier, 12 keycodes", show(set_(1, [200] + [0] * 11)))
    log("SetModifierMapping length 1", show(x.raw(118, 1, b"", units=1, reply=True)))
    set_(kpm, list(saved))
    x.mapping_events()


def hosts(x, log, restore_access):
    def lh():
        return x.raw(110, 0, reply=True)
    r = lh()
    n = struct.unpack_from(x.e + "H", r[1], 8)[0]
    log("ListHosts mode", r[1][1], "hosts", n)
    log("ListHosts length 2", fmt(x.raw(110, 0, b"\0\0\0\0", reply=True)))
    ch = lambda mode, fam, addr, units=None: x.raw(109, mode, x.p("BBH", fam, 0, len(addr)) + addr, units=units)
    log("ChangeHosts Insert Internet 10.9.8.7", fmt(ch(0, 0, bytes([10, 9, 8, 7]))))
    r = lh()
    log("  ListHosts hosts", struct.unpack_from(x.e + "H", r[1], 8)[0])
    log("ChangeHosts Delete Internet 10.9.8.7", fmt(ch(1, 0, bytes([10, 9, 8, 7]))))
    log("ChangeHosts mode 2", fmt(ch(2, 0, bytes([10, 9, 8, 7]))))
    log("ChangeHosts Internet, 3-byte address", fmt(ch(0, 0, bytes([10, 9, 8]))))
    log("ChangeHosts InternetV6, 4-byte address", fmt(ch(0, 6, bytes([10, 9, 8, 7]))))
    log("ChangeHosts family 1 (DECnet)", fmt(ch(0, 1, bytes([1, 2]))))
    log("ChangeHosts family 3 (unknown)", fmt(ch(0, 3, bytes([1, 2, 3, 4]))))
    log("ChangeHosts family 254 (Local)", fmt(ch(0, 254, b"")))
    log("ChangeHosts address longer than the request", fmt(ch(0, 0, bytes([10, 9, 8, 7]), units=2)))
    log("ChangeHosts mode 2 and too short", fmt(ch(2, 0, bytes([10, 9, 8, 7]), units=2)))
    log("ChangeHosts length 1", fmt(x.raw(109, 0, b"", units=1)))
    log("SetAccessControl mode 2", fmt(x.raw(111, 2)))
    log("SetAccessControl length 2", fmt(x.raw(111, 0, b"\0\0\0\0")))
    log("SetAccessControl Disable", fmt(x.raw(111, 0)))
    log("SetAccessControl Enable", fmt(x.raw(111, 1)))
    r = lh()
    log("  ListHosts mode", r[1][1])
    if restore_access is not None:
        x.raw(111, restore_access)


def close_down(port, msb, log, reset):
    a = X(port, msb)
    log("SetCloseDownMode 3", fmt(a.raw(112, 3)))
    log("SetCloseDownMode length 2", fmt(a.raw(112, 0, b"\0\0\0\0")))
    log("SetCloseDownMode RetainPermanent", fmt(a.raw(112, 1)))
    pm = a.new_id()
    a.raw(53, 16, a.p("IIHH", pm, a.root, 8, 8))  # CreatePixmap 8x8
    win = a.new_id()
    a.raw(1, 0, a.p("IIhhHHHHII", win, a.root, 0, 0, 10, 10, 0, 1, 0, 0))  # CreateWindow, InputOutput
    b = X(port, msb)
    a.close()
    time.sleep(0.3)
    geo = lambda c, d: fmt(c.raw(14, 0, c.p("I", d), reply=True))
    log("RetainPermanent: the pixmap after its client closed", geo(b, pm))
    log("RetainPermanent: the window after its client closed", geo(b, win))
    log("KillClient of the retained pixmap", fmt(b.raw(113, 0, b.p("I", pm))))
    log("  the pixmap after KillClient", geo(b, pm))
    log("  the window after KillClient", geo(b, win))
    log("KillClient of the same ID again", fmt(b.raw(113, 0, b.p("I", pm))))
    # RetainTemporary, then KillClient(AllTemporary)
    c = X(port, msb)
    c.raw(112, 2)
    pm2 = c.new_id()
    c.raw(53, 16, c.p("IIHH", pm2, c.root, 8, 8))
    d = X(port, msb)
    d.raw(112, 1)
    pm3 = d.new_id()
    d.raw(53, 16, d.p("IIHH", pm3, d.root, 8, 8))
    c.close()
    d.close()
    time.sleep(0.3)
    log("RetainTemporary pixmap after close", geo(b, pm2))
    log("KillClient AllTemporary", fmt(b.raw(113, 0, b.p("I", 0))))
    log("  temporary pixmap", geo(b, pm2))
    log("  permanent pixmap", geo(b, pm3))
    log("FreePixmap of a retained client's pixmap by another client", fmt(b.raw(54, 0, b.p("I", pm3))))
    log("  permanent pixmap", geo(b, pm3))
    # a live client's resource: KillClient closes it
    e = X(port, msb)
    pm4 = e.new_id()
    e.raw(53, 16, e.p("IIHH", pm4, e.root, 8, 8))
    log("KillClient of a live client's pixmap", fmt(b.raw(113, 0, b.p("I", pm4))))
    time.sleep(0.3)
    try:
        e.sync()
        alive = "still connected"
    except (ConnectionError, OSError, socket.timeout):
        alive = "closed"
    log("  the live client", alive)
    log("  its pixmap", geo(b, pm4))
    log("KillClient of an unknown ID", fmt(b.raw(113, 0, b.p("I", 0x0FFFFFFF))))
    # RetainPermanent client closes while another is connected; then the last connection
    # closes in Destroy mode: with a server reset the retained resources go too
    f = X(port, msb)
    f.raw(112, 1)
    pm5 = f.new_id()
    f.raw(53, 16, f.p("IIHH", pm5, f.root, 8, 8))
    f.close()
    time.sleep(0.2)
    log("retained pixmap, one connection left", geo(b, pm5))
    b.close()
    time.sleep(0.5)
    g = X(port, msb)
    log("retained pixmap after the last connection closed (Destroy)", geo(g, pm5))
    a2 = X(port, msb)  # the last connection closes in RetainPermanent mode: no reset
    a2.raw(112, 1)
    pm6 = a2.new_id()
    a2.raw(53, 16, a2.p("IIHH", pm6, a2.root, 8, 8))
    g.close()
    a2.close()
    time.sleep(0.5)
    h = X(port, msb)
    log("retained pixmap after the last connection closed (RetainPermanent)", geo(h, pm6))
    h.raw(113, 0, h.p("I", pm6))
    h.close()
    time.sleep(0.3)


def run(port, msb, reset):
    out = []

    def log(label, *a):
        out.append((label, " ".join(str(v) for v in a)))
    x = X(port, msb)
    keyboard(x, log)
    modifiers(x, log)
    r = x.raw(110, 0, reply=True)
    hosts(x, log, r[1][1])
    x.close()
    time.sleep(0.3)
    close_down(port, msb, log, reset)
    return out


def main():
    ref, test = int(sys.argv[1]), int(sys.argv[2])
    reset = "--reset" in sys.argv
    diffs = 0
    for msb in (False, True):
        a, b = run(ref, msb, reset), run(test, msb, reset)
        print("==== %s byte order" % ("MSB" if msb else "LSB"))
        for i in range(max(len(a), len(b))):
            l, r = (a[i] if i < len(a) else ("?", "")), (b[i] if i < len(b) else ("?", ""))
            mark = "  " if l == r else "!="
            diffs += l != r
            print("%s %-58s %-34s | %s" % (mark, l[0], l[1], r[1] if l[0] == r[0] else "%s: %s" % r))
    print("differences:", diffs)


if __name__ == "__main__":
    main()
