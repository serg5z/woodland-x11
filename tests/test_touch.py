"""Milestone 3A acceptance: the touchscreen as the X11 pointer, on the host server.

    python3 -m unittest -v tests/test_touch.py   (needs build/x11_host_server; blackbox, xclock)

Touch input is synthetic and deterministic: timestamped GT911-style samples on the server's
stdin (x11_host_server --input) go through the same touch filter as on the ESP32. Each
sample is acknowledged with the pointer state, so the test knows exactly what the server
did; it waits a little between samples so window managers can react as with a finger.

  * raw clients: motion over the root and a child window, event coordinates and button
    state, crossing events, QueryPointer, a lost touch never leaves Button1 down, and a
    pointer grab ends when its client exits;
  * real Blackbox with two real xclocks: tapping a clock focuses it, dragging a title bar
    moves the frame, tapping the close button closes that clock, and a grab held by another
    client that exits does not get in the way;
  * real twm (wm/twmrc): dragging a title bar moves the frame (twm grabs the server and the
    pointer while it moves)."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from test_wm import BLACKBOX, TWM, TWMRC, XCLOCK, unexpected_errors  # noqa: E402
from x11_raw import ROOT, Raw, decode_event  # noqa: E402

SERVER = os.path.join(ROOT_DIR, "build", "x11_host_server")
PRESS, RELEASE, MOTION, ENTER, LEAVE = 4, 5, 6, 7, 8
M_PRESS, M_RELEASE, M_ENTER, M_LEAVE, M_MOTION = 0x4, 0x8, 0x10, 0x20, 0x40
BUTTON1 = 0x100


class TouchServer:
    """The host server with synthetic touch input; the trace is drained continuously."""

    def __init__(self, display: int, trace: bool = True):
        args = [SERVER, "-d", str(display), "--input"] + (["-t"] if trace else ["-q"])
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True)
        assert "listening" in self.p.stderr.readline()
        self.log = []
        self.drain = threading.Thread(target=lambda: self.log.extend(iter(self.p.stderr.readline, "")), daemon=True)
        self.drain.start()
        self.display = display
        self.t = 0  # touch controller time, ms

    def command(self, line: str) -> dict:
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        ack = self.p.stdout.readline().split()
        assert ack and ack[0] == "ack", ack
        return {"x": int(ack[1]), "y": int(ack[2]), "buttons": int(ack[3]), "sprite": int(ack[4], 16),
                "grab": int(ack[5]), "frozen": int(ack[6])}

    def sample(self, points: int, x: int = 0, y: int = 0, pause: float = 0.03) -> dict:
        """One controller poll, 15 ms after the previous one."""
        self.t += 15
        a = self.command(f"touch {self.t} {points} {x} {y}")
        time.sleep(pause)
        return a

    def lift(self, pause: float = 0.03) -> dict:
        """No touch until the filter releases (60 ms)."""
        a = None
        for _ in range(5):
            a = self.sample(0, pause=pause)
        assert a["buttons"] == 0, a
        return a

    def tap(self, x: int, y: int) -> dict:
        for _ in range(3):
            self.sample(1, x, y)
        return self.lift()

    def drag(self, x1: int, y1: int, x2: int, y2: int, steps: int = 12) -> dict:
        for _ in range(3):
            self.sample(1, x1, y1)
        for i in range(1, steps + 1):
            self.sample(1, x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps, pause=0.05)
        for _ in range(3):
            self.sample(1, x2, y2)
        return self.lift()

    def stop(self) -> str:
        self.p.stdin.close()
        self.p.terminate()
        self.p.wait(timeout=10)
        self.drain.join(timeout=10)
        self.p.stdout.close()
        self.p.stderr.close()
        return "".join(self.log)


@unittest.skipUnless(os.path.exists(SERVER), "needs build/x11_host_server")
class PointerTest(unittest.TestCase):
    def setUp(self):
        self.srv = TouchServer(700 + os.getpid() % 100)

    def tearDown(self):
        log = self.srv.stop()
        self.assertNotIn("Bad", "".join(l for l in log.splitlines(True) if "->" in l and "Bad" in l))

    def test_motion_buttons_crossing_and_grab_release(self):
        a = Raw(6000 + self.srv.display)
        w = a.create_window(ROOT, 100, 100, 200, 100, M_PRESS | M_RELEASE | M_ENTER | M_LEAVE | M_MOTION)
        c = a.create_window(w, 20, 20, 50, 50, M_PRESS | M_RELEASE | M_ENTER | M_LEAVE | M_MOTION)
        a.map(c)
        a.map(w)
        a.select(ROOT, M_PRESS | M_RELEASE | M_MOTION | M_ENTER | M_LEAVE)
        a.sync()
        # finger down on the root, then onto the child (abs 120..170, 120..170)
        ack = self.srv.sample(1, 50, 40)
        self.assertEqual((ack["x"], ack["y"], ack["buttons"], ack["sprite"]), (50, 40, BUTTON1, ROOT))
        ack = self.srv.sample(1, 130, 135)
        self.assertEqual(ack["sprite"], c)
        self.srv.sample(1, 140, 150)
        self.srv.lift()
        evs = [decode_event(m) for m in a.poll_events()]
        presses = [e for e in evs if e["code"] == PRESS]
        self.assertEqual(len(presses), 1)
        self.assertEqual((presses[0]["window"], presses[0]["x"], presses[0]["y"], presses[0]["state"]), (ROOT, 50, 40, 0))
        # the pointer first moves to the touch point (no button yet), then the press starts an
        # automatic grab on the root: the drag over the child is reported there, with Button1
        motions = [e for e in evs if e["code"] == MOTION]
        self.assertEqual((motions[0]["root_x"], motions[0]["root_y"], motions[0]["state"]), (50, 40, 0))
        self.assertTrue(len(motions) == 3 and all(e["window"] == ROOT and e["state"] == BUTTON1 for e in motions[1:]),
                        motions)
        self.assertEqual(motions[-1]["child"], w)
        rel = [e for e in evs if e["code"] == RELEASE]
        self.assertEqual(len(rel), 1)
        self.assertEqual((rel[0]["root_x"], rel[0]["root_y"], rel[0]["state"]), (140, 150, BUTTON1))
        # crossing into the child during the automatic grab on the root: only the grab window
        # hears it (Leave, Inferior); the release ends the grab, and the Ungrab crossing takes
        # the pointer from the root into the child: Leave(root), Enter(w, Virtual), Enter(c, Ancestor)
        cross = [(e["code"], e["detail"], e["window"], e["mode"]) for e in evs if e["code"] in (ENTER, LEAVE)]
        self.assertEqual(cross, [(LEAVE, 2, ROOT, 0), (LEAVE, 2, ROOT, 2), (ENTER, 1, w, 2), (ENTER, 0, c, 2)])
        # a tap inside the child: delivered there, child-relative coordinates
        self.srv.tap(125, 160)
        evs = [decode_event(m) for m in a.poll_events()]
        p = [e for e in evs if e["code"] == PRESS][0]
        self.assertEqual((p["window"], p["x"], p["y"], p["root_x"], p["root_y"]), (c, 5, 40, 125, 160))
        q = a.query_pointer(w)
        self.assertEqual((q["root_x"], q["root_y"], q["win_x"], q["win_y"], q["child"], q["mask"]), (125, 160, 25, 60, c, 0))
        # the touch controller stops answering with the finger down: Button1 is released
        self.srv.sample(1, 130, 160)
        for _ in range(3):
            self.srv.t += 15
            ack = self.srv.command(f"fail {self.srv.t}")
        self.srv.t += 15
        ack = self.srv.command(f"fail {self.srv.t}")
        self.assertEqual(ack["buttons"], 0)
        evs = [decode_event(m) for m in a.poll_events()]
        self.assertEqual([e["code"] for e in evs if e["code"] in (PRESS, RELEASE)], [PRESS, RELEASE])
        # another client grabs the pointer, then exits: the grab ends, events flow again
        b = Raw(6000 + self.srv.display)
        self.assertEqual(b.grab_pointer(ROOT, M_PRESS | M_RELEASE), 0)
        self.assertGreaterEqual(self.srv.command("query")["grab"], 0)
        self.srv.tap(130, 160)
        self.assertEqual([e for e in a.poll_events() if e[0] == PRESS], [])
        b.close()
        deadline = time.time() + 3
        while self.srv.command("query")["grab"] >= 0 and time.time() < deadline:
            time.sleep(0.05)
        self.assertEqual(self.srv.command("query")["grab"], -1)
        self.srv.tap(130, 160)
        self.assertEqual(len([e for e in a.poll_events() if e[0] == PRESS]), 1)
        a.close()


@unittest.skipUnless(os.path.exists(SERVER) and XCLOCK and BLACKBOX, "needs the host server, xclock and blackbox")
class BlackboxTouchTest(unittest.TestCase):
    def test_blackbox_focus_drag_close(self):
        display = 850 + os.getpid() % 100
        srv = TouchServer(display)
        home = tempfile.mkdtemp(prefix="bb")
        env = dict(os.environ, DISPLAY=f"127.0.0.1:{display}.0", XAUTHORITY="/dev/null", LC_ALL="C", LANG="C",
                   HOME=home)
        procs = {}
        try:
            procs["wm"] = subprocess.Popen([BLACKBOX], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           text=True)
            time.sleep(2.0)
            for name, geo in (("left", "150x150+40+60"), ("right", "150x150+300+60")):
                procs[name] = subprocess.Popen([XCLOCK, "-analog", "-update", "1", "-geometry", geo], env=env,
                                               stderr=subprocess.PIPE, text=True)
                time.sleep(1.0)
            time.sleep(1.5)
            self.assertIsNone(procs["wm"].poll(), "blackbox exited")
            r = Raw(6000 + display)
            trace = "".join(srv.log)
            clocks = {}
            for name, xy in (("left", "0x3c0028"), ("right", "0x3c012c")):
                m = re.search(r"CreateWindow \(1\) (0x[0-9a-f]+) 0x26 " + xy + " ", trace)
                self.assertIsNotNone(m, name)
                clocks[name] = int(m.group(1), 16)

            def frame_of(w):
                while True:
                    parent, _ = r.query_tree(w)
                    if parent == ROOT:
                        return w
                    w = parent

            def title_parts(frame):
                """The title bar and its buttons, left to right, as (window, abs x, abs y, w, h)."""
                _, kids = r.query_tree(frame)
                title = min(kids, key=lambda k: r.geometry(k)[1])  # the child at the top
                parts = []
                for k in r.query_tree(title)[1]:
                    _, ax, ay = r.translate(k, ROOT)
                    gx, gy, gw, gh, _ = r.geometry(k)
                    parts.append((k, ax, ay, gw, gh))
                return title, sorted(parts, key=lambda p: p[1])

            left_frame, right_frame = frame_of(clocks["left"]), frame_of(clocks["right"])
            self.assertNotEqual(left_frame, clocks["left"], "left clock not reparented")
            # 1. tap the left clock's face: Blackbox gives it the focus
            self.assertNotEqual(r.get_input_focus(), clocks["left"])
            _, fx, fy = r.translate(clocks["left"], ROOT)
            srv.tap(fx + 75, fy + 75)
            time.sleep(0.5)
            self.assertEqual(r.get_input_focus(), clocks["left"], "tap did not focus the left clock")
            # 2. a grab by another client that exits must not stay behind
            g = Raw(6000 + display)
            self.assertEqual(g.grab_pointer(ROOT, M_PRESS | M_RELEASE), 0)
            g.close()
            deadline = time.time() + 3
            while srv.command("query")["grab"] >= 0 and time.time() < deadline:
                time.sleep(0.05)
            self.assertEqual(srv.command("query")["grab"], -1, "pointer grab outlived its client")
            # 3. drag the left title bar by (+30, +120): the frame follows
            title, parts = title_parts(left_frame)
            label = parts[1]  # iconify, label, maximize, close
            x0, y0, _, _, _ = r.geometry(left_frame)
            sx, sy = label[1] + label[3] // 2, label[2] + label[4] // 2
            ack = srv.drag(sx, sy, sx + 30, sy + 120)
            self.assertEqual(ack["buttons"], 0)
            time.sleep(0.7)
            x1, y1, _, _, _ = r.geometry(left_frame)
            self.assertEqual((x1 - x0, y1 - y0), (30, 120), "title-bar drag did not move the window")
            self.assertEqual(srv.command("query")["grab"], -1, "grab left after the drag")
            _, cx, cy = r.translate(clocks["left"], ROOT)
            self.assertEqual((cx - fx, cy - fy), (30, 120))
            # 4. tap the right clock's close button: Blackbox asks it to close (WM_DELETE_WINDOW)
            _, parts = title_parts(right_frame)
            close = parts[-1]
            srv.tap(close[1] + close[3] // 2, close[2] + close[4] // 2)
            procs["right"].wait(timeout=5)
            time.sleep(0.5)
            self.assertIsNone(procs["left"].poll(), "the other clock closed too")
            self.assertEqual(r.query_pointer()["mask"], 0, "Button1 stuck")
            self.assertIsNone(procs["wm"].poll(), "blackbox exited")
            r.close()
        finally:
            errs = {}
            for k, p in procs.items():
                if p.poll() is None:
                    p.terminate()
                errs[k] = p.communicate(timeout=10)[1] or ""
            log = srv.stop()
            shutil.rmtree(home, ignore_errors=True)
        for k, e in errs.items():
            self.assertNotIn("X Error", e, (k, e))
        # the raw clients here (slots 3, 4) make no errors; the others as in Milestone 2C
        bad = unexpected_errors(log)
        self.assertEqual(bad, [], bad)


@unittest.skipUnless(os.path.exists(SERVER) and XCLOCK and TWM, "needs the host server, xclock and twm")
class TwmTouchTest(unittest.TestCase):
    def test_twm_drag(self):
        display = 950 + os.getpid() % 40
        srv = TouchServer(display)
        home = tempfile.mkdtemp(prefix="twm")
        env = dict(os.environ, DISPLAY=f"127.0.0.1:{display}", XAUTHORITY="/dev/null", LC_ALL="C", LANG="C", HOME=home)
        procs = {}
        try:
            procs["wm"] = subprocess.Popen([TWM, "-f", TWMRC], env=env, stderr=subprocess.PIPE, text=True)
            time.sleep(2.0)
            procs["clock"] = subprocess.Popen([XCLOCK, "-analog", "-update", "1", "-geometry", "150x150+40+60"],
                                              env=env, stderr=subprocess.PIPE, text=True)
            time.sleep(2.0)
            r = Raw(6000 + display)
            m = re.search(r"CreateWindow \(1\) (0x[0-9a-f]+) 0x26 0x3c0028 ", "".join(srv.log))
            self.assertIsNotNone(m)
            clock = frame = int(m.group(1), 16)
            while r.query_tree(frame)[0] != ROOT:
                frame = r.query_tree(frame)[0]
            self.assertNotEqual(frame, clock, "clock not reparented")
            x0, y0 = r.geometry(frame)[:2]
            _, cx, cy = r.translate(clock, ROOT)
            sx, sy = cx + 70, cy - 8  # the title bar above the clock, right of the name
            self.assertEqual(srv.drag(sx, sy, sx + 40, sy + 100)["buttons"], 0)
            time.sleep(0.7)
            x1, y1 = r.geometry(frame)[:2]
            self.assertEqual((x1 - x0, y1 - y0), (40, 100), "title-bar drag did not move the window")
            self.assertEqual(srv.command("query")["grab"], -1)
            self.assertEqual(r.query_pointer()["mask"], 0)
            r.close()
        finally:
            errs = {}
            for k, p in procs.items():
                if p.poll() is None:
                    p.terminate()
                errs[k] = p.communicate(timeout=10)[1] or ""
            log = srv.stop()
            shutil.rmtree(home, ignore_errors=True)
        for k, e in errs.items():
            self.assertNotIn("X Error", e, (k, e))
        self.assertEqual(unexpected_errors(log), [])


@unittest.skipUnless(os.path.exists(SERVER), "needs build/x11_host_server")
class CalibrationToolTest(unittest.TestCase):
    def test_touch_targets_suggests_calibration(self):
        """tools/touch_targets.py with taps that land 10 px right and 6 px up of each target."""
        srv = TouchServer(990 + os.getpid() % 9, trace=False)
        tool = subprocess.Popen([sys.executable, os.path.join(ROOT_DIR, "tools", "touch_targets.py"), "--display",
                                 f"127.0.0.1:{srv.display}", "--no-auth", "--timeout", "10"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            lines = []
            for _ in range(5):
                line = tool.stdout.readline()
                lines.append(line)
                m = re.match(r"target \d/5: tap the cross at \((\d+), (\d+)\)", line)
                self.assertIsNotNone(m, line)
                srv.tap(int(m.group(1)) + 10, int(m.group(2)) - 6)
                lines.append(tool.stdout.readline())
            out, err = tool.communicate(timeout=10)
            text = "".join(lines) + out
            self.assertEqual(tool.returncode, 0, text + err)
            self.assertIn("error +10, -6", text)
            self.assertIn("TOUCHCAL 10 809 -6 473", text)
        finally:
            if tool.poll() is None:
                tool.kill()
                tool.communicate()
            srv.stop()


if __name__ == "__main__":
    unittest.main()
