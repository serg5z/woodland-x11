"""Standard Xauthority/Xlib authentication against the portable host server."""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "esp32" / "cyd"))
from test_host import SERVER  # noqa: E402
from cyd_xauthority import create_authority  # noqa: E402

XDPYINFO = shutil.which("xdpyinfo") or ("/opt/local/bin/xdpyinfo" if Path("/opt/local/bin/xdpyinfo").exists() else None)
XCLOCK = shutil.which("xclock") or ("/opt/local/bin/xclock" if Path("/opt/local/bin/xclock").exists() else None)
XAUTH = shutil.which("xauth") or ("/opt/local/bin/xauth" if Path("/opt/local/bin/xauth").exists() else None)
COOKIE = bytes(range(0x41, 0x51))


class AuthServer:
    def __init__(self, *args):
        self.display=150+os.getpid()%300
        self.port=6000+self.display
        self.p=subprocess.Popen([SERVER,"-d",str(self.display),*args],stderr=subprocess.PIPE,text=True)
        self.first=self.p.stderr.readline()
        if "listening" not in self.first:
            rest=self.p.communicate(timeout=3)[1]
            raise RuntimeError(self.first+rest)
    def close(self):
        if self.p.poll() is None:
            self.p.terminate()
        self.p.communicate(timeout=5)


@unittest.skipUnless(Path(SERVER).exists() and XAUTH and XDPYINFO, "build host server and install xauth/xdpyinfo")
class XauthorityIntegrationTest(unittest.TestCase):
    def run_info(self, server, authority):
        env = dict(os.environ, DISPLAY=f"127.0.0.1:{server.display}", XAUTHORITY=str(authority))
        return subprocess.run([XDPYINFO], env=env, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=6, check=False)

    def test_correct_missing_wrong_and_failed_setup_slot_reuse(self):
        server = AuthServer("--cookie", COOKIE.hex(), "--clients", "1")
        self.addCleanup(server.close)
        with tempfile.TemporaryDirectory(prefix="cyd-xauth-test-") as td:
            d = Path(td)
            good_hex = d / "good.cookie"
            wrong_hex = d / "wrong.cookie"
            good_hex.write_text(COOKIE.hex() + "\n")
            wrong_hex.write_text(bytes(reversed(COOKIE)).hex() + "\n")
            os.chmod(good_hex, 0o600); os.chmod(wrong_hex, 0o600)
            good_auth = d / "good.Xauthority"
            wrong_auth = d / "wrong.Xauthority"
            empty_auth = d / "empty.Xauthority"
            empty_auth.touch(mode=0o600)
            create_authority("127.0.0.1", server.display, good_hex, good_auth, xauth=XAUTH)
            create_authority("127.0.0.1", server.display, wrong_hex, wrong_auth, xauth=XAUTH)
            self.assertEqual(stat.S_IMODE(good_auth.stat().st_mode), 0o600)
            self.assertNotIn(COOKIE.hex(), " ".join([str(XAUTH), str(good_auth)]))

            missing = self.run_info(server, empty_auth)
            self.assertNotEqual(missing.returncode, 0, missing.stdout)
            wrong = self.run_info(server, wrong_auth)
            self.assertNotEqual(wrong.returncode, 0, wrong.stdout)
            # With a one-client server, the immediately following correct client proves
            # rejected setup freed its pending X11 client slot and transport buffers.
            correct = self.run_info(server, good_auth)
            self.assertEqual(correct.returncode, 0, correct.stderr)
            self.assertIn("version number:    11.0", correct.stdout)

    def test_authority_output_is_private_and_existing_file_is_not_overwritten(self):
        with tempfile.TemporaryDirectory(prefix="cyd-xauth-mode-") as td:
            d=Path(td); source=d/"cookie"; output=d/"authority"
            source.write_text(COOKIE.hex()+"\n");os.chmod(source,0o600)
            create_authority("127.0.0.1", 0, source, output, xauth=XAUTH)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode),0o600)
            with self.assertRaises(FileExistsError):
                create_authority("127.0.0.1",0,source,output,xauth=XAUTH)
            os.chmod(source,0o644)
            with self.assertRaises(PermissionError):
                create_authority("127.0.0.1",0,source,d/"insecure-output",xauth=XAUTH)

    @unittest.skipUnless(XCLOCK, "xclock required for two simultaneous native Xlib clients")
    def test_two_simultaneous_standard_xlib_clients(self):
        server = AuthServer("--cookie", COOKIE.hex(), "--clients", "4")
        self.addCleanup(server.close)
        with tempfile.TemporaryDirectory(prefix="cyd-xauth-multi-") as td:
            d = Path(td); cookie_file=d/"cookie"; authority=d/"authority"
            cookie_file.write_text(COOKIE.hex()+"\n");os.chmod(cookie_file,0o600)
            create_authority("127.0.0.1", server.display, cookie_file, authority, xauth=XAUTH)
            env=dict(os.environ,DISPLAY=f"127.0.0.1:{server.display}",XAUTHORITY=str(authority))
            clock=subprocess.Popen([XCLOCK,"-digital","-geometry","100x80+0+0"],env=env,
                                   stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
            try:
                time.sleep(.35)
                self.assertIsNone(clock.poll(), "xclock exited before second Xlib client connected")
                info=subprocess.run([XDPYINFO],env=env,text=True,stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE,timeout=6,check=False)
                self.assertEqual(info.returncode,0,info.stderr)
                self.assertIsNone(clock.poll(), "first authenticated Xlib client was disrupted")
            finally:
                clock.terminate()
                try: clock.wait(timeout=2)
                except subprocess.TimeoutExpired: clock.kill();clock.wait(timeout=2)
                if clock.stderr: clock.stderr.close()


if __name__ == "__main__":
    unittest.main()
