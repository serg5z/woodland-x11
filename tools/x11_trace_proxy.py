#!/usr/bin/env python3
"""X11 protocol tracer: a proxy between X clients and one X server (Xvfb over its Unix socket,
or the ESP32 panel over TCP), for comparing servers under the same clients.

    tools/x11_trace_proxy.py LISTEN_DISPLAY UPSTREAM LOG
        UPSTREAM: unix:/tmp/.X11-unix/X98  or  tcp:PANEL_IP:6000
    tools/x11_trace_proxy.py --make-xauth OUT DISPLAYS [--cookie-from HOST:DISPLAY]
        writes a mode-600 Xauthority file with one wildcard entry per display in DISPLAYS
        (comma separated) carrying the same MIT-MAGIC-COOKIE-1: a fresh random one, or the one
        ~/.Xauthority holds for HOST:DISPLAY. Nothing is printed.

The proxy listens on 127.0.0.1:(6000 + LISTEN_DISPLAY) only. Clients still authenticate to the
real server: the connection setup (which carries the cookie) is forwarded untouched and never
logged. Log lines (t = ms since the proxy started):
    t conn seq > Name [minor] len=BYTES [args]       a request (extension requests by name once
                                                      QueryExtension has told their opcode)
    t conn seq < reply len=BYTES dt=MS Name           a reply, with the time since its request
    t conn seq ! ErrorName Name[.minor] bad=0x..      an error, with the request that caused it
    t conn seq @ EventName                            an event (first 2000 per connection)
    t conn - = gap MS (N requests unanswered)         the server was silent this long while
                                                      replies were outstanding (>= 200 ms)
    t conn - x closed in=BYTES out=BYTES              the end of a connection
"""
from __future__ import annotations

import os
import secrets
import select
import socket
import struct
import sys
import threading
import time

CORE = [None, "CreateWindow", "ChangeWindowAttributes", "GetWindowAttributes", "DestroyWindow", "DestroySubwindows",
        "ChangeSaveSet", "ReparentWindow", "MapWindow", "MapSubwindows", "UnmapWindow", "UnmapSubwindows",
        "ConfigureWindow", "CirculateWindow", "GetGeometry", "QueryTree", "InternAtom", "GetAtomName",
        "ChangeProperty", "DeleteProperty", "GetProperty", "ListProperties", "SetSelectionOwner",
        "GetSelectionOwner", "ConvertSelection", "SendEvent", "GrabPointer", "UngrabPointer", "GrabButton",
        "UngrabButton", "ChangeActivePointerGrab", "GrabKeyboard", "UngrabKeyboard", "GrabKey", "UngrabKey",
        "AllowEvents", "GrabServer", "UngrabServer", "QueryPointer", "GetMotionEvents", "TranslateCoordinates",
        "WarpPointer", "SetInputFocus", "GetInputFocus", "QueryKeymap", "OpenFont", "CloseFont", "QueryFont",
        "QueryTextExtents", "ListFonts", "ListFontsWithInfo", "SetFontPath", "GetFontPath", "CreatePixmap",
        "FreePixmap", "CreateGC", "ChangeGC", "CopyGC", "SetDashes", "SetClipRectangles", "FreeGC", "ClearArea",
        "CopyArea", "CopyPlane", "PolyPoint", "PolyLine", "PolySegment", "PolyRectangle", "PolyArc", "FillPoly",
        "PolyFillRectangle", "PolyFillArc", "PutImage", "GetImage", "PolyText8", "PolyText16", "ImageText8",
        "ImageText16", "CreateColormap", "FreeColormap", "CopyColormapAndFree", "InstallColormap",
        "UninstallColormap", "ListInstalledColormaps", "AllocColor", "AllocNamedColor", "AllocColorCells",
        "AllocColorPlanes", "FreeColors", "StoreColors", "StoreNamedColor", "QueryColors", "LookupColor",
        "CreateCursor", "CreateGlyphCursor", "FreeCursor", "RecolorCursor", "QueryBestSize", "QueryExtension",
        "ListExtensions", "ChangeKeyboardMapping", "GetKeyboardMapping", "ChangeKeyboardControl",
        "GetKeyboardControl", "Bell", "ChangePointerControl", "GetPointerControl", "SetScreenSaver",
        "GetScreenSaver", "ChangeHosts", "ListHosts", "SetAccessControl", "SetCloseDownMode", "KillClient",
        "RotateProperties", "ForceScreenSaver", "SetPointerMapping", "GetPointerMapping", "SetModifierMapping",
        "GetModifierMapping"] + [None] * 6 + ["NoOperation"]
ERRORS = ["?", "BadRequest", "BadValue", "BadWindow", "BadPixmap", "BadAtom", "BadCursor", "BadFont", "BadMatch",
          "BadDrawable", "BadAccess", "BadAlloc", "BadColormap", "BadGC", "BadIDChoice", "BadName", "BadLength",
          "BadImplementation"]
EVENTS = [None, None, "KeyPress", "KeyRelease", "ButtonPress", "ButtonRelease", "MotionNotify", "EnterNotify",
          "LeaveNotify", "FocusIn", "FocusOut", "KeymapNotify", "Expose", "GraphicsExpose", "NoExpose",
          "VisibilityNotify", "CreateNotify", "DestroyNotify", "UnmapNotify", "MapNotify", "MapRequest",
          "ReparentNotify", "ConfigureNotify", "ConfigureRequest", "GravityNotify", "ResizeRequest",
          "CirculateNotify", "CirculateRequest", "PropertyNotify", "SelectionClear", "SelectionRequest",
          "SelectionNotify", "ColormapNotify", "ClientMessage", "MappingNotify", "GenericEvent"]
GAP_MS = 200
# core requests that have a reply (the others are only "answered" by a later reply or event)
REPLY_OPS = {3, 14, 15, 16, 17, 20, 21, 23, 26, 31, 38, 39, 40, 43, 44, 47, 48, 49, 50, 52, 73, 83, 84, 85, 86, 87,
             91, 92, 97, 98, 99, 101, 103, 106, 108, 110, 116, 117, 119}
REPLY_NAMES = {CORE[o] for o in REPLY_OPS}
T0 = time.monotonic()
lock = threading.Lock()


def now_ms() -> float:
    return (time.monotonic() - T0) * 1000


class Conn:
    def __init__(self, cid: int, log):
        self.cid, self.log = cid, log
        self.order = "<"
        self.cbuf, self.sbuf = b"", b""
        self.csetup = self.ssetup = False
        self.seq = 0
        self.pending: dict[int, tuple[str, float, str]] = {}  # seq16 -> (name, t, extra)
        self.ext: dict[int, str] = {}  # major opcode -> extension name
        self.qext: dict[int, str] = {}  # seq16 of QueryExtension -> name asked
        self.events = 0
        self.bytes_in = self.bytes_out = 0
        self.last_server = now_ms()

    def w(self, s: str) -> None:
        with lock:
            self.log.write("%.1f %d %s\n" % (now_ms(), self.cid, s))
            self.log.flush()

    def u16(self, b, o):
        return struct.unpack_from(self.order + "H", b, o)[0]

    def u32(self, b, o):
        return struct.unpack_from(self.order + "I", b, o)[0]

    def name(self, major: int, minor: int) -> str:
        if major < 128:
            return CORE[major] or "op%d" % major
        return "%s.%d" % (self.ext.get(major, "ext%d" % major), minor)

    def from_client(self, d: bytes) -> None:
        self.bytes_in += len(d)
        self.cbuf += d
        while True:
            b = self.cbuf
            if not self.csetup:
                if len(b) < 12:
                    return
                self.order = ">" if b[:1] == b"B" else "<"
                nl, dl = self.u16(b, 6), self.u16(b, 8)
                total = 12 + (nl + 3) // 4 * 4 + (dl + 3) // 4 * 4
                if len(b) < total:
                    return
                self.cbuf, self.csetup = b[total:], True  # authorization data: skipped, never logged
                self.w("- setup (authorization not logged)")
                continue
            if len(b) < 4:
                return
            n, hdr = self.u16(b, 2), 4
            if n == 0:  # BIG-REQUESTS
                if len(b) < 8:
                    return
                n, hdr = self.u32(b, 4), 8
            size = 4 * n
            if len(b) < size:
                return
            self.seq = (self.seq + 1) & 0xFFFF
            major, minor = b[0], b[1]
            nm = self.name(major, minor)
            extra = ""
            if major in (16, 98) and size >= 8:  # InternAtom / QueryExtension: the name
                ln = self.u16(b, hdr)
                txt = b[hdr + 4:hdr + 4 + ln].decode(errors="replace")
                extra = " " + txt
                if major == 98:
                    self.qext[self.seq] = txt
            elif major in (72, 73) and size >= hdr + 20:  # PutImage / GetImage: geometry, format, depth
                f = struct.unpack_from(self.order + "IIHHhhBB" if major == 72 else self.order + "IhhHHI", b, hdr)
                if major == 72:
                    extra = " fmt=%d drawable=0x%x gc=0x%x %dx%d at %d,%d leftpad=%d depth=%d" % ((minor,) + f)
                else:
                    extra = " fmt=%d drawable=0x%x %d,%d %dx%d mask=0x%x" % ((minor,) + f)
            elif size >= hdr + 4:
                words = [self.u32(b, o) for o in range(hdr, min(size, hdr + 12), 4)]
                extra = " " + " ".join("0x%x" % x for x in words)
            self.pending[self.seq] = (nm, now_ms(), extra)
            if len(self.pending) > 4096:
                self.pending.pop(next(iter(self.pending)))
            self.w("%d > %s len=%d%s" % (self.seq, nm, size, extra))
            self.cbuf = b[size:]

    def from_server(self, d: bytes) -> None:
        self.bytes_out += len(d)
        t = now_ms()
        waiting = [s for s, (nm, t0, _) in self.pending.items() if nm in REPLY_NAMES or "." in nm]
        if t - self.last_server >= GAP_MS and waiting:
            oldest = min(self.pending[s][1] for s in waiting)
            self.w("- = gap %.0f: server silent while %d reply-requests waited (oldest %.0f ms)" % (
                t - self.last_server, len(waiting), t - oldest))
        self.last_server = t
        self.sbuf += d
        while True:
            b = self.sbuf
            if not self.ssetup:
                if len(b) < 8:
                    return
                n = self.u16(b, 6)
                if len(b) < 8 + 4 * n:
                    return
                self.w("- setup %s" % {0: "FAILED", 1: "ok", 2: "authenticate"}.get(b[0], str(b[0])))
                self.sbuf, self.ssetup = b[8 + 4 * n:], True
                continue
            if len(b) < 32:
                return
            kind = b[0]
            if kind == 1:
                n = self.u32(b, 4)
                if len(b) < 32 + 4 * n:
                    return
                seq = self.u16(b, 2)
                nm, t0, _ = self.pending.get(seq, ("?", t, ""))
                if seq in self.qext:
                    present, major = b[8], b[9]
                    if present:
                        self.ext[major] = self.qext[seq]
                    self.w("%d < reply QueryExtension %s present=%d major=%d dt=%.1f" % (
                        seq, self.qext.pop(seq), present, major, t - t0))
                else:
                    self.w("%d < reply len=%d dt=%.1f %s" % (seq, 32 + 4 * n, t - t0, nm))
                self.forget(seq)
                self.sbuf = b[32 + 4 * n:]
            elif kind == 0:
                seq = self.u16(b, 2)
                bad, minor, major = self.u32(b, 4), self.u16(b, 8), b[10]
                err = ERRORS[b[1]] if b[1] < len(ERRORS) else "error%d" % b[1]
                self.w("%d ! %s %s bad=0x%x" % (seq, err, self.name(major, minor), bad))
                self.forget(seq)
                self.sbuf = b[32:]
            else:
                code = kind & 0x7F
                n = 32
                if code == 35:  # GenericEvent
                    n = 32 + 4 * self.u32(b, 4)
                    if len(b) < n:
                        return
                self.events += 1
                if self.events <= 2000:
                    self.w("%d @ %s%s" % (self.u16(b, 2), EVENTS[code] if code < len(EVENTS) and EVENTS[code]
                                          else "event%d" % code, " (sent)" if kind & 0x80 else ""))
                self.forget(self.u16(b, 2))
                self.sbuf = b[n:]

    def forget(self, seq: int) -> None:
        """Requests up to seq are processed (the server answers in order)."""
        for s in list(self.pending):
            if (seq - s) & 0xFFFF < 0x8000:
                del self.pending[s]


def connect(upstream: str) -> socket.socket:
    kind, _, rest = upstream.partition(":")
    if kind == "unix":
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(rest)
    else:
        host, _, port = rest.rpartition(":")
        s = socket.create_connection((host, int(port)))
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def serve(cid: int, client: socket.socket, upstream: str, log) -> None:
    c = Conn(cid, log)
    srv = connect(upstream)
    try:
        while True:
            r, _, _ = select.select([client, srv], [], [], 0.5)
            if not r:
                if c.pending and now_ms() - c.last_server >= GAP_MS:
                    pass  # reported when the server speaks again
                continue
            if client in r:
                d = client.recv(65536)
                if not d:
                    break
                srv.sendall(d)
                c.from_client(d)
            if srv in r:
                d = srv.recv(65536)
                if not d:
                    break
                client.sendall(d)
                c.from_server(d)
    except OSError as e:
        c.w("- x error %s" % e)
    finally:
        c.w("- x closed in=%d out=%d events=%d" % (c.bytes_in, c.bytes_out, c.events))
        client.close()
        srv.close()


def read_xauth(path: str):
    with open(path, "rb") as f:
        d = f.read()
    o = 0
    while o + 2 <= len(d):
        fam = struct.unpack_from(">H", d, o)[0]
        o += 2
        parts = []
        for _ in range(4):
            n = struct.unpack_from(">H", d, o)[0]
            parts.append(d[o + 2:o + 2 + n])
            o += 2 + n
        yield fam, *parts


def make_xauth(out: str, displays: str, cookie_from: str | None) -> None:
    cookie = None
    if cookie_from:
        host, _, disp = cookie_from.rpartition(":")
        addr = socket.inet_aton(socket.gethostbyname(host))
        for fam, a, num, name, data in read_xauth(os.path.expanduser("~/.Xauthority")):
            if name == b"MIT-MAGIC-COOKIE-1" and num == disp.encode() and (a == addr or a == host.encode()):
                cookie = data
        if cookie is None:
            sys.exit("no cookie for %s in ~/.Xauthority" % cookie_from)
    cookie = cookie or secrets.token_bytes(16)
    blob = b""
    for disp in displays.split(","):
        blob += struct.pack(">H", 0xFFFF)
        for part in (b"", disp.encode(), b"MIT-MAGIC-COOKIE-1", cookie):
            blob += struct.pack(">H", len(part)) + part
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(blob)


def main() -> None:
    if sys.argv[1:2] == ["--make-xauth"]:
        cf = sys.argv[sys.argv.index("--cookie-from") + 1] if "--cookie-from" in sys.argv else None
        make_xauth(sys.argv[2], sys.argv[3], cf)
        return
    display, upstream, log = int(sys.argv[1]), sys.argv[2], open(sys.argv[3], "a")
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", 6000 + display))
    ls.listen(8)
    n = 0
    while True:
        c, _ = ls.accept()
        n += 1
        threading.Thread(target=serve, args=(n, c, upstream, log), daemon=True).start()


if __name__ == "__main__":
    main()
