"""tools/touch_diag.py (P6 multi-touch diagnostics display): parsing the tracker's mt.* lines,
one distinct shape per slot, and a real render on the host X server (pixels checked)."""
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import touch_diag as td  # noqa: E402
from png_tools import read_png  # noqa: E402

SERVER = os.path.join(ROOT, "build", "x11_host_server")

SAMPLE = """esp.uptime_ms=5000
mt.seq=12
mt.count=2
mt.max_seen=2
mt.by_count=10,5,3,0,0,0
mt.reorders=1
mt.c0=0,200,240,30,MOVE
mt.c3=4,600,300,28,UP
mt.c9=bad
spi.state=on
"""


class Model(unittest.TestCase):
    def test_parse(self):
        contacts, counters = td.parse(SAMPLE)
        self.assertEqual([c["slot"] for c in contacts], [0, 3])
        self.assertEqual(contacts[1], {"slot": 3, "id": 4, "x": 600, "y": 300, "size": 28, "state": "UP"})
        self.assertEqual((counters["count"], counters["max_seen"], counters["reorders"]), ("2", "2", "1"))
        self.assertNotIn("uptime_ms", counters)
        self.assertNotIn("c9", counters)  # a malformed contact line is ignored, not a counter

    def test_one_shape_per_slot(self):
        kinds = [tuple(k for k, _ in td.shape_ops(s, 400, 240)[:-1]) + (td.SHAPES[s],) for s in range(5)]
        self.assertEqual(len(set(kinds)), 5)
        for s in range(5):
            self.assertEqual(td.shape_ops(s, 400, 240)[-1][0], "segments")  # every marker has a crosshair
        self.assertIn("triangle", td.label({"slot": 2, "id": 9, "x": 1, "y": 2, "size": 3, "state": "NEW"}))

    def test_gesture_lines(self):
        text = ("g.pairs=3\ng.scroll_v=1\ng.scroll_h=0\ng.pan=0\ng.pinch=1\ng.taps=1\ng.ends=2\ng.cancels=0\n"
                "g.ev4=100,two_down,none,400,240,0,0,1.00,0\ng.ev5=160,start,pinch,400,240,0,0,1.00,0\n"
                "g.update=300,update,pinch,402,241,2,1,1.32,2\n")
        contacts, counters = td.parse(text)
        lines = td.gesture_lines(counters)
        self.assertIn("pinch 1", lines[0])
        self.assertEqual(lines[1], "now: update pinch at 402,241 scale 1.32 steps 2")
        self.assertEqual(lines[2], "start pinch at 400,240")  # newest first
        self.assertEqual(td.gesture_lines({}), [])
        _, c2 = td.parse(text + "g.x11_button4=6\ng.x11_button5=2\ng.x11_button3=1\ng.x11_deferred=0\ng.x11_dropped=0\n")
        self.assertIn("X11: wheel up 6  wheel down 2  right click 1", td.gesture_lines(c2)[1])
        _, c3 = td.parse(text + "gate.hold_ms=50\ngate.taps=4\ngate.commits=2\ngate.discards=3\n"
                                "gate.latency_ms_last=45\ngate.latency_ms_max=60\n")
        self.assertIn("pointer gate 50 ms: taps 4  presses 2  kept from gestures 3  latency 45 ms (max 60)",
                      td.gesture_lines(c3)[1])
        _, c4 = td.parse(text + "gate.hold_ms=50\ngate.still_ms=150\ngate.move_px=10\ngate.taps=0\n")
        self.assertIn("pointer gate 50 ms (still 150 ms, moved 10 px): taps 0", td.gesture_lines(c4)[1])

    def test_header_without_data(self):
        lines = td.header({}, 12.0)
        self.assertEqual(len(lines), 3)
        self.assertIn("contacts -", lines[0])


@unittest.skipUnless(os.path.exists(SERVER), "make build/x11_host_server")
class Render(unittest.TestCase):
    def test_render_on_host_server(self):
        tmp = tempfile.mkdtemp()
        fake, png = os.path.join(tmp, "diag.txt"), os.path.join(tmp, "fb.png")
        pts = [(0, 120, 200), (1, 330, 200), (2, 540, 200), (3, 220, 360), (4, 440, 360)]
        with open(fake, "w") as f:
            f.write("mt.seq=3\nmt.count=5\nmt.max_seen=5\n")
            for slot, x, y in pts:
                f.write(f"mt.c{slot}={slot + 2},{x},{y},30,{'UP' if slot == 4 else 'MOVE'}\n")
        srv = subprocess.Popen([SERVER, "-d", "178", "--png", png, "-q"], stderr=subprocess.PIPE, text=True)
        try:
            time.sleep(0.4)
            cli = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "touch_diag.py"), "--display",
                                    "127.0.0.1:178", "--no-auth", "--fake", fake, "--seconds", "1.5"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(1.0)  # the server rewrites its PNG on every change (at most every 200 ms)
            with open(png, "rb") as f:
                shown = f.read()  # the window as shown, before the client exits
            out, err = cli.communicate(timeout=30)
            self.assertEqual(cli.returncode, 0, out + err)
        finally:
            srv.terminate()
            srv.wait(timeout=10)
        with open(png, "wb") as f:
            f.write(shown)
        w, h, rows = read_png(png)
        self.assertEqual((w, h), (800, 480))
        red, green, cyan, yellow = (255, 0, 0), (0, 255, 0), (0, 255, 255), (255, 255, 0)  # full-range expansion
        R = td.R
        self.assertEqual(rows[200][120], red)                 # slot 0: crosshair centre
        self.assertEqual(rows[200][120 + R], red)             # circle: right edge on the centre line
        self.assertEqual(rows[200 - R][120], red)             # circle: top
        self.assertNotEqual(rows[200 - R + 4][120 - R + 4], red)  # inside the circle's corner: empty
        self.assertEqual(rows[200 - R][330 - R], green)       # slot 1: square corner
        self.assertEqual(rows[200 - R][540], cyan)            # slot 2: triangle apex
        self.assertEqual(rows[360][220 + R - 1], yellow)      # slot 3: diamond right vertex (the join leaves the tip pixel)
        self.assertNotIn(rows[360 - R][440 - R], (red, green, cyan, yellow, (255, 0, 255)))  # slot 4 UP: grey
        self.assertNotEqual(rows[360 - R][440 - R], rows[5][400])   # but drawn
        label_row = [rows[y][x] for y in range(200 + R + 4, 200 + R + 18) for x in range(80, 300)]
        self.assertTrue(any(p == (255, 255, 255) for p in label_row), "white label text under slot 0")


if __name__ == "__main__":
    unittest.main()


class Chain(unittest.TestCase):
    """The chain's lines (tc.*) and the trails; a live run against simulated frames."""

    def test_transitions_and_counters(self):
        text = ("mt.overflow=1\ntc.frames=20\ntc.failed_frames=2\ntc.points_in=31\ntc.out_of_range=1\n"
                "tc.transitions=3\ntc.frame_us_max=40\ntc.ev0=3,45,BEGIN,7,0,100,200\ntc.ev1=9,135,END,7,0,140,210\n"
                "tc.ev2=12,180,LOST,2,1,300,300\ntc.ev3=bad\n")
        _, counters = td.parse(text)
        t = td.transitions(counters)
        self.assertEqual([(e["frame"], e["kind"], e["id"]) for e in t], [(3, "BEGIN", 7), (9, "END", 7), (12, "LOST", 2)])
        lines = td.transition_lines(counters)
        self.assertIn("frames 20  failed 2", lines[0])
        self.assertIn("overflow 1", lines[0])
        self.assertTrue(lines[1].startswith("frame 12 (180 ms): LOST id2 #1"))  # newest first
        self.assertEqual(td.transition_lines({}), [])

    def test_trails(self):
        c = lambda slot, id_, x, y, st="MOVE": {"slot": slot, "id": id_, "x": x, "y": y, "size": 1, "state": st}
        tr = {}
        for i in range(30):
            tr = td.trail_update(tr, [c(0, 7, i, i), c(1, 3, 100, 100)], 24)
        self.assertEqual(len(tr[(0, 7)]), 24)                 # bounded
        self.assertEqual(tr[(0, 7)][-1], (29, 29))
        self.assertEqual(tr[(1, 3)], [(100, 100)])            # a still contact: one point
        tr = td.trail_update(tr, [c(0, 9, 5, 5)], 24)         # a new ID in slot 0; slot 1 lifted
        self.assertEqual(tr, {(0, 9): [(5, 5)]})
        self.assertEqual(td.trail_update(tr, [c(0, 9, 6, 6, "UP")], 24), {})
        self.assertEqual(td.trail_update(tr, [c(0, 9, 6, 6)], 0), {})

    @unittest.skipUnless(os.path.exists(SERVER), "build the server first (make)")
    def test_live_with_simulated_frames(self):
        srv = subprocess.Popen([SERVER, "-d", "179", "--input", "-q"], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            time.sleep(0.4)
            cli = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "touch_diag.py"), "--display",
                                    "127.0.0.1:179", "--no-auth", "--hz", "40", "--seconds", "2"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            t = 0
            for i in range(40):  # three fingers sweeping right for ~1.5 s
                t += 15
                pts = " ".join(f"{k + 1} {100 + 10 * i} {120 + 100 * k}" for k in range(3))
                srv.stdin.write(f"mt {t} 3 {pts}\n")
                srv.stdin.flush()
                srv.stdout.readline()
                time.sleep(0.035)
            out, err = cli.communicate(timeout=30)
            self.assertEqual(cli.returncode, 0, out + err)
            self.assertGreaterEqual(json.loads(out.strip().splitlines()[-1])["redraws"], 5)
        finally:
            srv.stdin.close()
            srv.terminate()
            srv.wait(timeout=10)
