"""Host viewer wire, framebuffer and pointer regression. No GUI or hardware required."""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from x11_raw import ROOT, Raw, decode_event

ROOT_DIR = Path(__file__).resolve().parents[1]
SERVER = ROOT_DIR / "build/x11_host_server"


def exact(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise ConnectionError("viewer socket closed")
        data.extend(part)
    return bytes(data)


def frame(sock):
    head = exact(sock, 16)
    magic, width, height, rev, length = struct.unpack("<4sHHII", head)
    assert magic == b"WLV1" and length == width * height * 2
    return width, height, rev, exact(sock, length)


@unittest.skipUnless(SERVER.exists(), "build host server first")
class ViewerLinkTest(unittest.TestCase):
    def test_frame_pointer_release_and_reconnect(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "viewer.sock")
            display = 480 + os.getpid() % 80
            server = subprocess.Popen([str(SERVER), "-d", str(display), "--viewer-socket", path, "-q"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            viewer = None
            raw = None
            try:
                self.assertIn("listening", server.stderr.readline())
                viewer = socket.socket(socket.AF_UNIX)
                viewer.settimeout(3)
                viewer.connect(path)
                width, height, _, pixels = frame(viewer)
                self.assertEqual((width, height, len(pixels)), (800, 480, 768000))
                raw = Raw(6000 + display)
                win = raw.create_window(ROOT, 20, 20, 100, 80, 0x4 | 0x8 | 0x40, bg=0)
                raw.map(win)
                gc = raw.create_gc(win, 0xf800)
                started = time.monotonic()
                raw.fill_rects(win, gc, [(10, 10, 20, 20)])
                raw.sync()
                until = time.monotonic() + 3
                while time.monotonic() < until:
                    _, _, _, pixels = frame(viewer)
                    if struct.unpack_from("<H", pixels, (35 * 800 + 35) * 2)[0] == 0xf800:
                        break
                else:
                    self.fail("RGB565 red rectangle did not reach viewer")
                self.assertLess(time.monotonic() - started, 0.5, "viewer frame stalled")
                viewer.sendall(b"pointer 35 35 1\npointer 35 35 0\n")
                time.sleep(0.1)
                events = [decode_event(e) for e in raw.poll_events()]
                self.assertEqual([e["code"] for e in events if e["code"] in (4, 5)], [4, 5])
                self.assertEqual(raw.query_pointer()["mask"], 0)
                viewer.sendall(b"pointer 35 35 4\npointer 35 35 0\n"
                               b"pointer 35 35 2\npointer 35 35 0\n"
                               b"pointer 35 35 8\npointer 35 35 0\n"
                               b"pointer 35 35 16\npointer 35 35 0\n")
                time.sleep(0.1)
                buttons = [(e["code"], e["detail"]) for e in map(decode_event, raw.poll_events())
                           if e["code"] in (4, 5)]
                self.assertEqual(buttons, [(4, 3), (5, 3), (4, 2), (5, 2),
                                           (4, 4), (5, 4), (4, 5), (5, 5)])
                self.assertEqual(raw.query_pointer()["mask"], 0)
                viewer.sendall(b"pointer 35 35 1\n")
                time.sleep(0.05)
                viewer.close(); viewer = None
                time.sleep(0.1)
                self.assertEqual(raw.query_pointer()["mask"], 0, "disconnect left button held")
                viewer = socket.socket(socket.AF_UNIX)
                viewer.settimeout(3); viewer.connect(path)
                self.assertEqual(frame(viewer)[:2], (800, 480))
            finally:
                if raw: raw.close()
                if viewer: viewer.close()
                server.terminate()
                server.communicate(timeout=3)
