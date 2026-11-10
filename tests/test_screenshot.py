"""tools/screenshot.py against the host server: the PNG it writes over the network equals,
pixel for pixel, the server's own framebuffer PNG (--png), including partial bottom bands."""
import os
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
from png_tools import read_png  # noqa: E402
from x11_raw import ROOT as ROOT_WINDOW, Raw  # noqa: E402

SERVER = os.path.join(ROOT, "build", "x11_host_server")
TOOL = os.path.join(ROOT, "tools", "screenshot.py")


@unittest.skipUnless(os.path.exists(SERVER), "make build/x11_host_server")
class Screenshot(unittest.TestCase):
    def setUp(self):
        self.display = 880 + os.getpid() % 100
        self.tmp = tempfile.mkdtemp()
        self.fb = os.path.join(self.tmp, "fb.png")
        self.srv = subprocess.Popen([SERVER, "-d", str(self.display), "--png", self.fb, "-q"],
                                    stderr=subprocess.DEVNULL)
        time.sleep(0.4)

    def tearDown(self):
        self.srv.terminate()
        self.srv.wait(timeout=10)

    def shoot(self, *args):
        out = os.path.join(self.tmp, "shot.png")
        r = subprocess.run([sys.executable, TOOL, "--display", f"127.0.0.1:{self.display}", "--no-auth",
                            "--out", out, *args], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return read_png(out)

    def test_matches_framebuffer(self):
        c = Raw(6000 + self.display)
        # three colours (red, green, blue in RGB565) so the byte order and channels show
        for i, bg in enumerate((0xF800, 0x07E0, 0x001F)):
            c.map(c.create_window(ROOT_WINDOW, 40 + 200 * i, 100 + 30 * i, 150, 250, bg=bg))
        c.sync()
        time.sleep(0.6)  # the server writes its PNG at most every 200 ms
        _, _, want = read_png(self.fb)
        for band in ("96", "77", "480"):  # 77: a partial last band
            w, h, got = self.shoot("--band", band)
            self.assertEqual((w, h), (800, 480))
            self.assertEqual(got, want, f"band {band}")
        self.assertEqual(got[150][100], (255, 0, 0))
        self.assertEqual(got[200][300], (0, 255, 0))
        self.assertEqual(got[250][500], (0, 0, 255))
        c.close()

    def test_needs_display(self):
        r = subprocess.run([sys.executable, TOOL, "--out", os.path.join(self.tmp, "x.png")],
                           capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--display", r.stderr)


if __name__ == "__main__":
    unittest.main()
