"""Host server end to end: the compiled x11_host_server against the independent Python
client (both byte orders, auth, fragments, requests and errors) and, when
installed, the real xdpyinfo: no X errors, sensible values, the documented request trace,
and several clients at once."""
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from x11_client import connect, recv_exact  # noqa: E402

SERVER = os.path.join(ROOT, "build", "x11_host_server")
XDPYINFO = shutil.which("xdpyinfo") or ("/opt/local/bin/xdpyinfo" if os.path.exists("/opt/local/bin/xdpyinfo") else None)
COOKIE = bytes(range(0xA0, 0xB0))
# What xdpyinfo (libX11 + libxcb, default options) sends after setup, verified on macOS
# (MacPorts) and Debian.
XDPYINFO_EXPECTED = [
    "version number:    11.0", "vendor string:    Woodland Lab experimental X11 (ESP32-S3)",
    "maximum request size:  16384 bytes", "bitmap unit, bit order, padding:    32, LSBFirst, 32",
    "depth 16, bits_per_pixel 16, scanline_pad 32", "keycode range:    minimum 8, maximum 255",
    "focus:  PointerRoot", "number of extensions:    0",
    "dimensions:    800x480 pixels (95x54 millimeters)", "depths (2):    16, 1",
    "root window id:    0x26", "default colormap:    0x20", "default number of colormap cells:    64",
    "preallocated pixels:    black 0, white 65535", "largest cursor:    32x32",
    "class:    TrueColor", "red, green, blue masks:    0xf800, 0x7e0, 0x1f"]
XDPYINFO_TRACE = ["QueryExtension BIG-REQUESTS", "CreateGC", "GetProperty", "QueryExtension XKEYBOARD",
                  "GetInputFocus", "ListExtensions", "QueryBestSize", "FreeGC", "GetInputFocus"]


class Server:
    def __init__(self, *args):
        self.display = 150 + os.getpid() % 300
        self.port = 6000 + self.display
        self.p = subprocess.Popen([SERVER, "-d", str(self.display), *args], stderr=subprocess.PIPE, text=True)
        self.first = self.p.stderr.readline()  # "listening on 127.0.0.1:..." once bound
        if "listening" not in self.first:
            raise RuntimeError(self.first)

    def log(self) -> str:
        if self.p.poll() is None:
            self.p.terminate()
        return self.first + self.p.communicate(timeout=5)[1]


@unittest.skipUnless(os.path.exists(SERVER), "build the server first (make)")
class HostServerTest(unittest.TestCase):
    def setUp(self):
        self.srv = None

    def tearDown(self):
        if self.srv and self.srv.p.poll() is None:
            self.srv.log()

    def start(self, *args):
        self.srv = Server(*args)
        return self.srv

    def test_success_both_byte_orders(self):
        srv = self.start()
        for msb in (False, True):
            s, rep = connect(srv.port, msb)
            s.close()
            self.assertEqual(rep["status"], "success")
            self.assertEqual(rep["version"], [11, 0])
            self.assertEqual(rep["trailing_bytes"], 0)
            sc = rep["screens"][0]
            self.assertEqual((sc["width_px"], sc["height_px"], sc["root_depth"]), (800, 480, 16))
            vis = {v["id"]: (d["depth"], v) for d in sc["depths"] for v in d["visuals"]}
            depth, root_vis = vis[sc["root_visual"]]
            self.assertEqual(depth, 16)
            self.assertEqual((root_vis["class"], root_vis["masks"]), (4, [0xF800, 0x07E0, 0x001F]))
            self.assertEqual({f["depth"] for f in rep["formats"]}, {d["depth"] for d in sc["depths"]})
            self.assertEqual(rep["resource_id_base"] & rep["resource_id_mask"], 0)
            self.assertLess(max(sc["root"], sc["default_colormap"], sc["root_visual"]), rep["resource_id_base"])

    def test_fragmented_request(self):
        srv = self.start("--cookie", COOKIE.hex())
        s, rep = connect(srv.port, True, b"MIT-MAGIC-COOKIE-1", COOKIE, chunks=1)
        s.close()
        self.assertEqual(rep["status"], "success")

    def test_auth(self):
        srv = self.start("--cookie", COOKIE.hex())
        cases = [(b"", b"", "authorization required"),
                 (b"MIT-MAGIC-COOKIE-1", bytes(16), "invalid MIT-MAGIC-COOKIE-1"),
                 (b"XDM-AUTHORIZATION-1", COOKIE, "unsupported authorization protocol")]
        for name, data, reason in cases:
            for msb in (False, True):
                s, rep = connect(srv.port, msb, name, data)
                s.close()
                self.assertEqual(rep["status"], "failed")
                self.assertIn(reason, rep["reason"])
                self.assertTrue(rep["padding_zero"])
        s, rep = connect(srv.port, False, b"MIT-MAGIC-COOKIE-1", COOKIE)
        s.close()
        self.assertEqual(rep["status"], "success")

    def test_no_cookie_server_rejects_credentials(self):
        srv = self.start()
        s, rep = connect(srv.port, False, b"MIT-MAGIC-COOKIE-1", COOKIE)
        s.close()
        self.assertEqual(rep["status"], "failed")

    def test_bad_version(self):
        srv = self.start()
        s, rep = connect(srv.port, True, major=10)
        s.close()
        self.assertEqual(rep["status"], "failed")
        self.assertIn("protocol version", rep["reason"])

    def test_requests_in_both_byte_orders(self):
        srv = self.start()
        for msb in (False, True):
            e = ">" if msb else "<"
            s, rep = connect(srv.port, msb)
            s.sendall(struct.pack(e + "BBHHH", 98, 0, 5, 9, 0) + b"XKEYBOARD\0\0\0")  # QueryExtension
            s.sendall(struct.pack(e + "BBH", 43, 0, 1))                 # GetInputFocus
            s.sendall(struct.pack(e + "BBH", 127, 0, 1))                # NoOperation: no reply
            s.sendall(struct.pack(e + "BBHI", 109, 0, 2, 0))            # ChangeHosts: BadAccess
            r = recv_exact(s, 32)
            self.assertEqual((r[0], struct.unpack(e + "H", r[2:4])[0], r[8]), (1, 1, 0))  # reply, seq 1, absent
            r = recv_exact(s, 32)
            self.assertEqual((r[0], struct.unpack(e + "H", r[2:4])[0]), (1, 2))
            self.assertEqual(struct.unpack(e + "I", r[8:12])[0], 1)     # PointerRoot
            r = recv_exact(s, 32)                                       # seq 3 had no reply
            self.assertEqual((r[0], r[1], struct.unpack(e + "H", r[2:4])[0], r[10]), (0, 10, 4, 109))
            s.close()

    def test_getimage_stream_backpressure_other_client_and_event_order(self):
        srv = self.start("--clients", "6", "--budget-mb", "4", "--outq-kb", "96")
        first, setup = connect(srv.port, False)
        root = setup["screens"][0]["root"]
        first.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        # Select PropertyNotify on root, then request the complete screen image (768,000 bytes).
        first.sendall(struct.pack("<BBHIII", 2, 0, 4, root, 1 << 11, 1 << 22))
        first.sendall(struct.pack("<BBHIhhHHI", 73, 2, 5, root, 0, 0, 800, 480, 0xFFFF))
        reply = recv_exact(first, 32)
        self.assertEqual((reply[0], struct.unpack("<H", reply[2:4])[0]), (1, 2))
        self.assertEqual(struct.unpack("<I", reply[4:8])[0] * 4, 768000)

        # A second client must still make progress while the first client reads slowly.
        second, setup2 = connect(srv.port, False)
        second.sendall(struct.pack("<BBH", 43, 0, 1))
        focus = recv_exact(second, 32)
        self.assertEqual((focus[0], struct.unpack("<H", focus[2:4])[0]), (1, 1))

        # A server-generated event to the streaming client must follow the complete reply.
        second.sendall(struct.pack("<BBHIIIB3xI", 18, 0, 7, root, 31, 31, 8, 1) + b"Z\0\0\0")
        atom_name = b"_WOODLAND_DIAG"
        second.sendall(struct.pack("<BBHHH", 16, 0, 6, len(atom_name), 0) + atom_name + b"\0" * 2)
        diag_atom = struct.unpack("<I", recv_exact(second, 32)[8:12])[0]
        second.sendall(struct.pack("<BBHIIIII", 20, 0, 6, root, diag_atom, 0, 0, 4096))
        diag_reply = recv_exact(second, 32)
        diag_payload = recv_exact(second, struct.unpack("<I", diag_reply[4:8])[0] * 4)
        live_diag = dict(line.split("=", 1) for line in diag_payload.decode().splitlines() if "=" in line)
        self.assertLessEqual(int(live_diag["net.queue_peak_max"]), 1024)
        image_bytes = image_crc = 0
        while image_bytes < 768000:
            try:
                data = recv_exact(first, min(8192, 768000 - image_bytes))
                image_bytes += len(data)
                image_crc = zlib.crc32(data, image_crc)
            except TimeoutError as e:
                keys = ("net.bytes_out", "net.queue_bytes", "net.queue_peak_max", "net.conn.0")
                snapshot = {key: live_diag.get(key) for key in keys}
                raise AssertionError(f"stream stalled after {image_bytes} payload bytes; diag={snapshot}") from e
        self.assertEqual(image_bytes, 768000)
        self.assertNotEqual(image_crc, 0)  # payload contains framebuffer data
        event = recv_exact(first, 32)
        self.assertEqual(event[0] & 0x7F, 28)  # PropertyNotify follows GetImage payload
        self.assertEqual(struct.unpack("<H", event[2:4])[0], 2)
        # The diagnostic reports bounded connection buffers; the logical image is over 768 KiB.
        self.assertLess(int(live_diag["net.conn_bytes_peak"]), 768000)
        first.close()
        second.close()

    def test_getimage_stream_disconnect_does_not_break_other_clients(self):
        srv = self.start("--clients", "6", "--budget-mb", "4", "--outq-kb", "96")
        first, setup = connect(srv.port, False)
        root = setup["screens"][0]["root"]
        first.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        first.sendall(struct.pack("<BBHIhhHHI", 73, 2, 5, root, 0, 0, 800, 480, 0xFFFF))
        header = recv_exact(first, 32)
        self.assertEqual(struct.unpack("<I", header[4:8])[0] * 4, 768000)
        recv_exact(first, 1024)
        first.close()  # reset/close while the logical reply is still being produced
        second, _ = connect(srv.port, False)
        second.sendall(struct.pack("<BBH", 43, 0, 1))
        reply = recv_exact(second, 32)
        self.assertEqual((reply[0], struct.unpack("<H", reply[2:4])[0]), (1, 1))
        second.close()

    def test_cairo_593x3_tightly_packed_putimage_is_bad_length(self):
        srv = self.start()
        s, setup = connect(srv.port, False)
        root = setup["screens"][0]["root"]
        gc = setup["resource_id_base"]
        fmt16 = next(f for f in setup["formats"] if f["depth"] == 16)
        self.assertEqual((fmt16["bpp"], fmt16["scanline_pad"]), (16, 32))

        # Captured from GTK3/Cairo: PutImage ZPixmap, 593x3, depth 16,
        # left-pad 0, 3584 bytes total. Cairo supplies tightly packed 1186-byte
        # rows and only two bytes of request-end padding, rather than 1188-byte rows.
        s.sendall(struct.pack("<BBHIII", 55, 0, 4, gc, root, 0))  # CreateGC, seq 1
        pixels = bytes((i * 17) & 0xFF for i in range(593 * 3 * 2))
        malformed = struct.pack("<BBHIIHHhhBBH", 72, 2, 896, root, gc, 593, 3, 212, 120, 0, 16, 0)
        malformed += pixels + b"\0\0"
        self.assertEqual(len(malformed), 3584)
        s.sendall(malformed)                                        # PutImage, seq 2
        s.sendall(struct.pack("<BBH", 43, 0, 1))                   # GetInputFocus, seq 3
        err, focus = recv_exact(s, 32), recv_exact(s, 32)
        self.assertEqual((err[0], err[1], struct.unpack("<H", err[2:4])[0], err[10]), (0, 16, 2, 72))
        self.assertEqual((focus[0], struct.unpack("<H", focus[2:4])[0]), (1, 3))

        # Two pad bytes belong on every scanline. The correctly padded request is accepted.
        row = pixels[:593 * 2] + b"\0\0"
        valid = struct.pack("<BBHIIHHhhBBH", 72, 2, 897, root, gc, 593, 3, 212, 120, 0, 16, 0)
        valid += row * 3
        self.assertEqual(len(valid), 3588)
        s.sendall(valid)                                             # PutImage, seq 4
        s.sendall(struct.pack("<BBH", 43, 0, 1))                   # GetInputFocus, seq 5
        focus = recv_exact(s, 32)
        self.assertEqual((focus[0], struct.unpack("<H", focus[2:4])[0]), (1, 5))
        s.close()

    def test_length_zero_is_bad_length_and_client_stays(self):
        # Without BIG-REQUESTS a length of 0 is just too short: BadLength for the 4-byte header
        # (XTS <Request>-2 for one-word requests), and the connection goes on.
        srv = self.start()
        a, _ = connect(srv.port)
        b, _ = connect(srv.port, True)
        a.sendall(struct.pack("<BBH", 43, 0, 0))                        # length 0: BIG-REQUESTS form
        r = recv_exact(a, 32)
        self.assertEqual((r[0], r[1], r[10]), (0, 16, 43))              # BadLength
        self.assertEqual(struct.unpack_from("<H", r, 2)[0], 1)
        a.sendall(struct.pack("<BBH", 43, 0, 1))
        r = recv_exact(a, 32)
        self.assertEqual((r[0], struct.unpack_from("<H", r, 2)[0]), (1, 2))  # still served
        b.sendall(struct.pack(">BBH", 43, 0, 1))
        self.assertEqual(recv_exact(b, 32)[0], 1)                       # the other client is fine
        a.close()
        b.close()

    def test_too_long_request_is_bad_length_and_client_stays(self):
        # X11 "Request Format": a length above maximum-request-length is a Length error. The
        # request's bytes are discarded (here sent in pieces) and the connection stays (XTS
        # assertion <Request>-3 for every core request).
        srv = self.start()
        for msb in (False, True):
            e = ">" if msb else "<"
            s, _ = connect(srv.port, msb)
            units = 4097                                                 # one above 16384 bytes
            req = struct.pack(e + "BBH", 8, 0, units) + struct.pack(e + "I", 0x26) + bytes(4 * units - 8)
            for i in range(0, len(req), 5000):
                s.sendall(req[i:i + 5000])
                time.sleep(0.01)
            r = recv_exact(s, 32)
            self.assertEqual((r[0], r[1], r[10]), (0, 16, 8))           # BadLength for MapWindow
            self.assertEqual(struct.unpack_from(e + "H", r, 2)[0], 1)
            s.sendall(struct.pack(e + "BBH", 43, 0, 1))                  # GetInputFocus still works
            r = recv_exact(s, 32)
            self.assertEqual((r[0], struct.unpack_from(e + "H", r, 2)[0]), (1, 2))
            s.close()

    def round_trip_ms(self, s):
        t0 = time.monotonic()
        s.sendall(struct.pack("<BBH", 43, 0, 1))  # GetInputFocus
        recv_exact(s, 32)
        return (time.monotonic() - t0) * 1000

    def test_slow_reader_gets_backpressure_not_the_others(self):
        srv = self.start("-q")
        slow, rep = connect(srv.port)
        fid = rep["resource_id_base"] | 1
        slow.sendall(struct.pack("<BBHIHH", 45, 0, 5, fid, 5, 0) + b"fixed\0\0\0")  # OpenFont
        n = 20000
        flood = struct.pack("<BBHI", 47, 0, 2, fid) * n  # QueryFont: 1,200-byte replies
        sender = threading.Thread(target=lambda: slow.sendall(flood))  # blocks once the server stops reading
        sender.start()
        time.sleep(1.0)  # the slow client's replies pile up; it never reads
        fast, _ = connect(srv.port, True)
        times = []
        for _ in range(50):
            t0 = time.monotonic()
            fast.sendall(struct.pack(">BBH", 43, 0, 1))
            recv_exact(fast, 32)
            times.append((time.monotonic() - t0) * 1000)
        self.assertLess(max(times), 200, times)  # the server loop is not stalled
        # now the slow client reads: every reply arrives, in order, none lost
        got = 0
        while got < n:
            r = recv_exact(slow, 32)
            self.assertEqual(r[0], 1)
            self.assertEqual(struct.unpack("<H", r[2:4])[0], (got + 2) & 0xFFFF)  # seq 1 was OpenFont
            recv_exact(slow, struct.unpack("<I", r[4:8])[0] * 4)
            got += 1
        sender.join(10)
        self.assertEqual(got, n)
        slow.close()
        fast.close()

    def test_partial_request_does_not_spin(self):
        srv = self.start("-q")
        s, _ = connect(srv.port)
        s.sendall(struct.pack("<BBH", 20, 0, 6) + b"\0" * 8)  # half a GetProperty, then silence
        time.sleep(0.2)
        cpu0 = self._server_cpu(srv)
        time.sleep(1.5)
        self.assertLess(self._server_cpu(srv) - cpu0, 0.3)  # the server sleeps in select()
        s.sendall(b"\0" * 12)  # the rest: GetProperty(window 0) -> BadWindow
        r = recv_exact(s, 32)
        self.assertEqual((r[0], r[1]), (0, 3))
        s.close()

    @staticmethod
    def _server_cpu(srv):
        out = subprocess.run(["ps", "-o", "time=", "-p", str(srv.p.pid)], capture_output=True, text=True).stdout.strip()
        parts = [float(x) for x in out.replace("-", ":").split(":")]
        sec = 0.0
        for v in parts:
            sec = sec * 60 + v
        return sec

    def test_event_overflow_closes_only_the_silent_client(self):
        srv = self.start("-q")
        silent, rep = connect(srv.port)
        root = rep["screens"][0]["root"]
        # ChangeWindowAttributes(root, event-mask = PropertyChangeMask), then never read again
        silent.sendall(struct.pack("<BBHIII", 2, 0, 4, root, 1 << 11, 0x400000))
        silent.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        time.sleep(0.3)
        busy, _ = connect(srv.port)
        prop = struct.pack("<BBHIIIBxxxI", 18, 0, 7, root, 39, 31, 8, 4) + b"tick"  # ChangeProperty WM_NAME
        for _ in range(30):
            busy.sendall(prop * 1000)  # 30,000 PropertyNotify events for the silent client
            busy.sendall(struct.pack("<BBH", 43, 0, 1))
            recv_exact(busy, 32)  # the busy client keeps getting answers
        self.assertLess(self.round_trip_ms(busy), 200)
        busy.close()
        log = srv.log()
        self.assertIn("output queue full", log)
        self.assertEqual(log.count("closed (output queue full"), 1)  # only the silent client
        silent.close()

    @unittest.skipUnless(XDPYINFO, "xdpyinfo not installed")
    def test_real_xdpyinfo(self):
        srv = self.start("--once")
        env = dict(os.environ, DISPLAY=f"127.0.0.1:{srv.display}", XAUTHORITY="/dev/null")
        out = subprocess.run([XDPYINFO], env=env, capture_output=True, text=True, timeout=30)
        log = srv.log()
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(out.stderr, "")                                # no X Error at all
        for line in XDPYINFO_EXPECTED:
            self.assertIn(line, out.stdout)
        trace = [m.group(1) + (" " + m.group(2) if m.group(2) else "")
                 for m in re.finditer(r'#\d+ (\w+) \(\d+\)(?: "([^"]+)")? len \d+ -> ', log)]
        self.assertEqual(trace, XDPYINFO_TRACE)
        self.assertNotRegex(log, r"-> Bad")
        self.assertIn("after 9 requests, 0 errors", log)

    @unittest.skipUnless(XDPYINFO, "xdpyinfo not installed")
    def test_concurrent_xdpyinfo(self):
        srv = self.start()
        env = dict(os.environ, DISPLAY=f"127.0.0.1:{srv.display}", XAUTHORITY="/dev/null")
        held = [connect(srv.port, msb)[0] for msb in (False, True)]  # two idle clients stay connected
        procs = [subprocess.Popen([XDPYINFO], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                 for _ in range(4)]
        outs = [p.communicate(timeout=30) for p in procs]
        for p, (so, se) in zip(procs, outs):
            self.assertEqual((p.returncode, se), (0, ""))
            self.assertIn("focus:  PointerRoot", so)
        for s in held:
            s.close()
        log = srv.log()
        bases = set(re.findall(r"resource ids (0x[0-9a-f]+)", log))
        self.assertGreaterEqual(len(bases), 2)                           # separate ID ranges in use


if __name__ == "__main__":
    unittest.main()
