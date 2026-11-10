"""Milestone 2C acceptance: real, unmodified window managers (twm, Blackbox) manage two real
xclock windows on the host server.

    python3 -m unittest -v tests/test_wm.py   (needs build/x11_host_server, twm/blackbox, xclock)

For each window manager:
  * it starts and keeps running; no X error reaches any client;
  * both clocks are reparented into frames, and decorations appear above each face;
  * another client unmaps one clock: the window manager unmanages it (the face is gone);
    the client maps it again: MapRequest, the window manager manages it again (face back);
  * a clock is closed: its frame goes away and the area shows the root again, the other
    clock is intact;
  * the window manager exits: the save-set puts the remaining clock back on the root, mapped.
Protocol errors in the server log must be in the allowed set, each with its reason (the
same errors a reference X server, Xvfb without RENDER at the panel's DPI, gives these
clients)."""
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
from png_tools import read_png  # noqa: E402
from x11_client import connect  # noqa: E402

SERVER = os.path.join(ROOT, "build", "x11_host_server")
TWMRC = os.path.join(ROOT, "wm", "twmrc")


def tool(name):
    return shutil.which(name) or (f"/opt/local/bin/{name}" if os.path.exists(f"/opt/local/bin/{name}") else None)


XCLOCK, TWM, BLACKBOX = tool("xclock"), tool("twm"), tool("blackbox")
WHITE = (255, 255, 255)
RENDER_PROBE_DEPTHS = {4, 8, 24, 32}  # libXrender's deliberate CreatePixmap probes
ERR = re.compile(r"\[(\d)\] #\d+ (\w+) \(\d+\)[^\n]*\(bad value 0x([0-9a-f]+)\)[^\n]* -> (Bad\w+)")


def white_box(rows, x1, x2):
    """The clock face in columns [x1, x2): the connected white region holding the longest
    white run of the middle column (title bars and toolbars are not part of it). Returns its
    bounding box, or None."""
    cx = (x1 + x2) // 2
    best, run, start = (0, None), 0, 0
    for y, row in enumerate(rows):
        if row[cx] == WHITE:
            if not run:
                start = y
            run += 1
            if run > best[0]:
                best = (run, start)
        else:
            run = 0
    if best[0] < 60:  # a 150-pixel face has a long white run through the middle
        return None
    seed = (cx, best[1] + best[0] // 2)
    seen, todo = {seed}, [seed]
    while todo:
        x, y = todo.pop()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if x1 <= nx < x2 and 0 <= ny < len(rows) and (nx, ny) not in seen and rows[ny][nx] == WHITE:
                seen.add((nx, ny))
                todo.append((nx, ny))
    xs, ys = [p[0] for p in seen], [p[1] for p in seen]
    return (min(xs), min(ys), max(xs) + 1, max(ys) + 1)


def unexpected_errors(log, wm_base=0x200000):
    """Protocol errors in a server trace that the reference server would not give these
    clients. wm_base: the window manager's resource-ID base (slot 0, started first)."""
    bad = []
    for slot, req, value, code in ERR.findall(log):
        v = int(value, 16)
        if req == "CreatePixmap" and code == "BadValue" and v in RENDER_PROBE_DEPTHS:
            continue  # libXrender's depth probes
        if req == "FreePixmap" and code == "BadPixmap":
            continue  # ... and freeing the probe pixmaps that were not created
        if code == "BadWindow" and slot == "0" and (v & ~0x1FFFFF) != wm_base:
            continue  # the window manager acting on a clock window that was just unmapped/destroyed
        if req == "GetImage" and code == "BadMatch" and slot == "0":
            continue  # Xft's core-text fallback reading an unviewable window (Xvfb without RENDER: same)
        bad.append((req, code, v, slot))
    return bad


class WindowManagerTest(unittest.TestCase):
    def scenario(self, name, argv, display_screen=False):
        d = tempfile.mkdtemp(prefix="wm")
        png = os.path.join(d, "fb.png")
        display = 600 + os.getpid() % 150 + (0 if name == "twm" else 150)
        srv = subprocess.Popen([SERVER, "-d", str(display), "-t", "--png", png], stderr=subprocess.PIPE, text=True)
        self.assertIn("listening", srv.stderr.readline())
        trace = []  # the server's trace, drained continuously (it is large)
        drain = threading.Thread(target=lambda: trace.extend(iter(srv.stderr.readline, "")), daemon=True)
        drain.start()
        dpy = f"127.0.0.1:{display}" + (".0" if display_screen else "")
        env = dict(os.environ, DISPLAY=dpy, XAUTHORITY="/dev/null", LC_ALL="C", LANG="C", HOME=d)
        procs = {}
        try:
            procs["wm"] = subprocess.Popen(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(2.0)
            args = [XCLOCK, "-analog", "-update", "1"]
            procs["left"] = subprocess.Popen(args + ["-geometry", "150x150+40+60"], env=env, stderr=subprocess.PIPE,
                                             text=True)
            time.sleep(1.0)
            procs["right"] = subprocess.Popen(args + ["-geometry", "150x150+300+60"], env=env, stderr=subprocess.PIPE,
                                              text=True)
            time.sleep(2.5)
            self.assertIsNone(procs["wm"].poll(), f"{name} exited")
            _, _, rows = read_png(png)
            root_px = rows[470][790]
            left, right = white_box(rows, 0, 230), white_box(rows, 260, 490)
            self.assertIsNotNone(left, "left clock face")
            self.assertIsNotNone(right, "right clock face")
            for box in (left, right):  # decorations: a title bar or frame right above the face
                x1, y1, x2, _ = box
                above = [rows[y][x] for y in range(max(0, y1 - 12), y1 - 2) for x in range(x1 + 10, x2 - 10)]
                self.assertTrue(any(p != root_px for p in above), (name, "no decoration above", box))

            # the right clock's top-level window: CreateWindow on the root (0x26) at (300, 60)
            m = re.search(r"CreateWindow \(1\) (0x[0-9a-f]+) 0x26 0x3c012c ", "".join(trace))
            self.assertIsNotNone(m, "right clock window in the trace")
            right_win = int(m.group(1), 16)
            ctl = connect(6000 + display)
            sock = ctl[0] if isinstance(ctl, tuple) else ctl

            def request(op, wid):  # a request on another client's window, then a round trip
                sock.sendall(struct.pack("<BBHI", op, 0, 2, wid) + struct.pack("<BBH", 43, 0, 1))
                reply = b""
                while len(reply) < 32:
                    reply += sock.recv(32 - len(reply))
                self.assertEqual(reply[0], 1, "round trip")

            # unmap by another client: the window manager unmanages (withdraws) the clock
            request(10, right_win)
            time.sleep(1.5)
            _, _, rows = read_png(png)
            self.assertIsNone(white_box(rows, 260, 490), "unmapped clock still shown")
            self.assertEqual(rows[right[1] + 20][right[0] + 20], root_px, "root not re-exposed")
            # map again: MapRequest to the window manager, which manages it again
            request(8, right_win)
            time.sleep(2.0)
            _, _, rows = read_png(png)
            self.assertIsNotNone(white_box(rows, 260, 490), "remapped clock not shown")
            sock.close()

            # close the right clock: frame gone, root re-exposed, left clock intact
            procs["right"].terminate()
            procs["right"].communicate(timeout=10)
            time.sleep(1.5)
            _, _, rows = read_png(png)
            self.assertIsNone(white_box(rows, 260, 490), "closed clock still shown")
            self.assertTrue(all(rows[y][x] == root_px for y in range(right[1] - 20, right[3]) for x in range(right[0], right[2])),
                            "frame left behind")
            self.assertEqual(white_box(rows, 0, 230), left, "left clock disturbed")

            # the window manager exits: save-set restore, the left clock stays visible on the root
            procs["wm"].terminate()
            procs["wm"].wait(timeout=10)
            time.sleep(1.5)
            _, _, rows = read_png(png)
            self.assertIsNotNone(white_box(rows, 0, 230), "save-set: clock lost when the window manager exited")
        finally:
            errs = {}
            for k, p in procs.items():
                if p.poll() is None:
                    p.terminate()
                errs[k] = p.communicate(timeout=10)[1] or ""
            srv.terminate()
            srv.wait(timeout=10)
            drain.join(timeout=10)
            shutil.rmtree(d, ignore_errors=True)
        for k, e in errs.items():
            self.assertNotIn("X Error", e, (k, e))
        self.check_errors(name, "".join(trace))

    def check_errors(self, name, log):
        bad = unexpected_errors(log)
        if bad:
            self.fail("%s: unexpected error %s %s (bad value 0x%x, client %s)" % ((name,) + bad[0]))

    @unittest.skipUnless(os.path.exists(SERVER) and XCLOCK and TWM, "needs the host server, xclock and twm")
    def test_twm(self):
        self.scenario("twm", [TWM, "-f", TWMRC])

    @unittest.skipUnless(os.path.exists(SERVER) and XCLOCK and BLACKBOX, "needs the host server, xclock and blackbox")
    def test_blackbox(self):
        # host:N.S: Blackbox derives its rootCommand DISPLAY by cutting the name at the last '.'
        self.scenario("blackbox", [BLACKBOX], display_screen=True)


if __name__ == "__main__":
    unittest.main()
