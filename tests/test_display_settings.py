"""tools/display_settings.py (LCD pixel-clock Display Settings): request texts, state parsing,
and against the host X server: the request property it writes and the window it draws."""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import display_settings as ds  # noqa: E402
from png_tools import read_png  # noqa: E402
from touch_diag import X, pad, req  # noqa: E402

SERVER = os.path.join(ROOT, "build", "x11_host_server")
TOOL = os.path.join(ROOT, "tools", "display_settings.py")

DIAG = """esp.uptime_ms=1
pclk.applied_mhz=14
pclk.confirmed_mhz=16
pclk.stored_confirmed=16
pclk.trial=1
pclk.restricted=0
pclk.boot_note=trial_started
pclk.last=none
lcd.fps_nominal=34.1
touch.on=1
"""


class Model(unittest.TestCase):
    def test_request_text(self):
        self.assertEqual(ds.request_text(14, 77), "request 14 77")
        self.assertEqual(ds.request_text(None, 5), "reset 5")
        with self.assertRaises(ValueError):
            ds.request_text(18, 1)

    def test_state(self):
        st = ds.pclk_state(DIAG)
        self.assertEqual(st["pclk.applied_mhz"], "14")
        self.assertNotIn("touch.on", st)
        self.assertIn("TRIAL", ds.describe(st))
        self.assertIn("34.1 fps", ds.describe(st))
        self.assertIn("not a -DX11_PCLK build", ds.describe({}))


def root_property(display, name):
    x = X("127.0.0.1", 6000 + display, None)
    n = name.encode()
    r = x.reply(x.send(req(16, 1, struct.pack("<H2x", len(n)) + pad(n))))
    atom = struct.unpack_from("<I", r, 8)[0]
    try:
        return x.get_text_property(atom) if atom else None
    finally:
        x.s.close()


@unittest.skipUnless(os.path.exists(SERVER), "make build/x11_host_server")
class HostServer(unittest.TestCase):
    def setUp(self):
        self.display = 860 + os.getpid() % 100
        self.png = os.path.join(tempfile.mkdtemp(), "fb.png")
        self.srv = subprocess.Popen([SERVER, "-d", str(self.display), "--png", self.png, "-q"], stderr=subprocess.DEVNULL)
        time.sleep(0.4)

    def tearDown(self):
        self.srv.terminate()
        self.srv.wait(timeout=10)

    def tool(self, *args, **kw):
        return subprocess.run([sys.executable, TOOL, "--display", f"127.0.0.1:{self.display}", "--no-auth", *args],
                              capture_output=True, text=True, timeout=30, **kw)

    def test_request_and_reset_write_the_property(self):
        r = self.tool("--request", "14")
        self.assertEqual(r.returncode, 0, r.stderr)
        sent = json.loads(r.stdout)["sent"]
        self.assertRegex(sent, r"^request 14 \d+$")
        self.assertEqual(root_property(self.display, "_WOODLAND_PCLK_REQUEST"), sent)
        r = self.tool("--reset")
        sent2 = json.loads(r.stdout)["sent"]
        self.assertRegex(sent2, r"^reset \d+$")
        self.assertEqual(root_property(self.display, "_WOODLAND_PCLK_REQUEST"), sent2)
        r = self.tool("--request", "18")  # not offered: refused by the tool
        self.assertNotEqual(r.returncode, 0)

    def test_status_without_the_feature(self):
        r = self.tool("--status")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("not a -DX11_PCLK build", json.loads(r.stdout)["summary"])

    def test_window(self):
        cli = subprocess.Popen([sys.executable, TOOL, "--display", f"127.0.0.1:{self.display}", "--no-auth",
                                "--seconds", "2"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(1.2)
        with open(self.png, "rb") as f:
            shown = f.read()
        out, err = cli.communicate(timeout=30)
        self.assertEqual(cli.returncode, 0, out + err)
        # A file of its own: the server is still running and rewrites self.png when the tool's
        # window goes away, which could replace the screenshot before it is read.
        shot = os.path.join(os.path.dirname(self.png), "shown.png")
        with open(shot, "wb") as f:
            f.write(shown)
        w, h, rows = read_png(shot)
        x0, y0, x1, y1 = ds.Window.BTN[16]  # without the feature: 16 MHz selected
        self.assertEqual(rows[y0 + 5][x0 + 5], (0, 158, 255))
        x0, y0, x1, y1 = ds.Window.BTN[12]
        self.assertNotEqual(rows[y0 + 5][x0 + 5], (0, 158, 255))
        x0, y0, x1, y1 = ds.Window.BTN["apply"]
        self.assertEqual(rows[y0 + 5][x0 + 5], (0, 255, 0))
        x0, y0, x1, y1 = ds.Window.BTN["reset"]
        self.assertEqual(rows[y0 + 5][x0 + 5], (255, 0, 0))


if __name__ == "__main__":
    unittest.main()
