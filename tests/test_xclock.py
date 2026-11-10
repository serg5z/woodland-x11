"""Milestone 2A acceptance: the real, unmodified xclock (analog, seconds hand) against the
host server, rendering into the 800x480 RGB565 framebuffer (read back as PNG snapshots).

    python3 -m unittest -v tests/test_xclock.py        (needs build/x11_host_server and xclock)

Checks: no X error reaches the client; the only protocol errors in the server log are
libXrender's deliberate pixmap-depth probes; a clock face with tick marks and hands appears
where xclock asked for its window; the picture changes as time passes; the amount of ink on
the face stays the same from frame to frame (old hands are erased, no artifacts pile up).
Set XCLOCK_SNAPSHOT_DIR to keep cropped snapshots."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
from png_tools import bbox, crop, read_png, write_png  # noqa: E402

SERVER = os.path.join(ROOT, "build", "x11_host_server")
XCLOCK = shutil.which("xclock") or ("/opt/local/bin/xclock" if os.path.exists("/opt/local/bin/xclock") else None)
WHITE = (255, 255, 255)
# libXrender's XRenderFindDisplay probes these pixmap depths under its own error handler
RENDER_PROBE_DEPTHS = {4, 8, 24, 32}


def dark(p):
    return sum(p) < 200


@unittest.skipUnless(os.path.exists(SERVER) and XCLOCK, "needs build/x11_host_server (make) and xclock")
class XclockTest(unittest.TestCase):
    def test_real_xclock(self):
        d = tempfile.mkdtemp(prefix="xclock")
        png = os.path.join(d, "fb.png")
        display = 200 + os.getpid() % 200
        srv = subprocess.Popen([SERVER, "-d", str(display), "--png", png], stderr=subprocess.PIPE, text=True)
        self.assertIn("listening", srv.stderr.readline())
        env = dict(os.environ, DISPLAY=f"127.0.0.1:{display}", XAUTHORITY="/dev/null", LC_ALL="C")
        clock = subprocess.Popen([XCLOCK, "-analog", "-update", "1", "-geometry", "220x220+300+120"], env=env,
                                 stderr=subprocess.PIPE, text=True)
        frames = []
        try:
            time.sleep(2.0)
            for i in range(6):  # six frames, 1.1 s apart
                frames.append(read_png(png))
                time.sleep(1.1)
        finally:
            clock.terminate()
            err = clock.communicate(timeout=10)[1]
            time.sleep(0.4)
            srv.terminate()
            log = srv.communicate(timeout=10)[1]
        keep = os.environ.get("XCLOCK_SNAPSHOT_DIR")
        try:
            self.assertNotIn("X Error", err, err)
            # errors in the server log: only the depth probes (CreatePixmap + matching FreePixmap)
            errors = re.findall(r"#\d+ (\w+) \(\d+\) \(bad value 0x([0-9a-f]+)\).* -> (Bad\w+)", log)
            for req, bad, code in errors:
                ok = (req == "CreatePixmap" and code == "BadValue" and int(bad, 16) in RENDER_PROBE_DEPTHS) or \
                     (req == "FreePixmap" and code == "BadPixmap")
                self.assertTrue(ok, (req, bad, code))
            self.assertLessEqual(len(errors), 8)
            # the window is where xclock asked for it: outer corner (300, 120) with Xt's default
            # 1-pixel border, so the white 220x220 inside starts at (301, 121)
            w, h, rows = frames[-1]
            self.assertEqual((w, h), (800, 480))
            box = bbox(rows, lambda p: p == WHITE)
            self.assertEqual(box, (301, 121, 521, 341), box)
            self.assertTrue(dark(rows[120][300]) and dark(rows[341][521]), "border")
            # ink inside the face: tick marks and hands
            inks = []
            for _, _, r in frames:
                face = crop(r, 301, 121, 521, 341)
                inks.append(sum(dark(p) for row in face for p in row))
            self.assertTrue(all(i > 300 for i in inks), inks)
            # the tick marks: the ring between 80% and 95% of the radius has ink all around
            face = crop(rows, 301, 121, 521, 341)
            quadrants = [0, 0, 0, 0]
            for y, row in enumerate(face):
                for x, p in enumerate(row):
                    dx, dy = x - 110, y - 110
                    r2 = dx * dx + dy * dy
                    if dark(p) and (0.80 * 110) ** 2 < r2 < (0.97 * 110) ** 2:
                        quadrants[(dx >= 0) + 2 * (dy >= 0)] += 1
            self.assertTrue(all(q > 20 for q in quadrants), quadrants)
            # time passes: frames differ (the seconds hand moves at least)
            faces = [crop(r, 301, 121, 521, 341) for _, _, r in frames]
            changed = sum(1 for a, b in zip(faces, faces[1:]) if a != b)
            self.assertGreaterEqual(changed, 4, inks)
            # no artifacts: the ink stays within a narrow band (old hands erased each second)
            self.assertLess(max(inks) - min(inks), 0.15 * min(inks), inks)
            if keep:
                os.makedirs(keep, exist_ok=True)
                write_png(os.path.join(keep, "xclock-host.png"), rows)
                write_png(os.path.join(keep, "xclock-host-face-1.png"), faces[0])
                write_png(os.path.join(keep, "xclock-host-face-2.png"), faces[-1])
                with open(os.path.join(keep, "xclock-trace.log"), "w") as f:
                    f.write(log)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_overlapping_clocks_stack_and_reexpose(self):
        d = tempfile.mkdtemp(prefix="xclock2")
        png = os.path.join(d, "fb.png")
        display = 400 + os.getpid() % 200
        srv = subprocess.Popen([SERVER, "-d", str(display), "--png", png, "-q"], stderr=subprocess.PIPE, text=True)
        self.assertIn("listening", srv.stderr.readline())
        env = dict(os.environ, DISPLAY=f"127.0.0.1:{display}", XAUTHORITY="/dev/null", LC_ALL="C")
        args = [XCLOCK, "-analog", "-update", "1"]
        bottom = subprocess.Popen(args + ["-geometry", "200x200+100+100"], env=env, stderr=subprocess.PIPE, text=True)
        time.sleep(1.5)
        top = subprocess.Popen(args + ["-geometry", "200x200+200+150"], env=env, stderr=subprocess.PIPE, text=True)
        try:
            time.sleep(2.0)
            _, _, rows = read_png(png)
            # the later window is on top: its border runs through the earlier clock's face
            self.assertTrue(dark(rows[150][250]) and dark(rows[200][200]), "top window border over the bottom one")
            top.terminate()
            top.communicate(timeout=10)
            time.sleep(1.5)  # the bottom clock gets Expose for the uncovered part and redraws
            _, _, rows = read_png(png)
            face = crop(rows, 101, 101, 301, 301)
            quadrants = [0, 0, 0, 0]
            for y, row in enumerate(face):
                for x, p in enumerate(row):
                    dx, dy = x - 100, y - 100
                    if dark(p) and (0.80 * 100) ** 2 < dx * dx + dy * dy < (0.97 * 100) ** 2:
                        quadrants[(dx >= 0) + 2 * (dy >= 0)] += 1
            # the lower-right quadrant was covered by the top clock: its ticks are back
            self.assertTrue(all(q > 15 for q in quadrants), quadrants)
            # and nothing of the closed clock remains outside the bottom one
            self.assertTrue(all(rows[y][x] == rows[470][790] for y in range(305, 350) for x in range(305, 400)))
        finally:
            for p in (top, bottom):
                if p.poll() is None:
                    p.terminate()
                err = p.communicate(timeout=10)[1]
                self.assertNotIn("X Error", err)
            srv.terminate()
            srv.communicate(timeout=10)
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
