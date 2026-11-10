"""Client scalability and stress tests of the host server built with AddressSanitizer and
UndefinedBehaviorSanitizer (build/x11_host_server_san, the same core and transport as the
ESP32). Each test reads the server's own counters from _WOODLAND_DIAG and checks that
everything returns to the baseline afterwards; the server must exit without sanitizer reports.

    python3 -m unittest -v tests/test_scale.py      (make first)

Covered: 128 clients with resource-ID isolation; graceful refusal past the client limit; a
connection burst; slow readers next to a busy client; disconnect/reconnect cycles; the
per-client resource quota; memory-budget exhaustion and recovery; a client reset while events
are queued for it. The long benchmark (60 s per level) is tools/x11_bench.py."""
from __future__ import annotations

import os
import re
import socket
import struct
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x11_client import connect, recv_exact  # noqa: E402
from x11_raw import ROOT, Raw  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(HERE, "build", "x11_host_server_san")
ID_UNIT = 0x200000


class Server:
    def __init__(self, *args):
        self.display = 400 + os.getpid() % 300
        self.port = 6000 + self.display
        env = dict(os.environ, ASAN_OPTIONS="abort_on_error=0:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        self.p = subprocess.Popen([SERVER, "-d", str(self.display), "-q", *args], stderr=subprocess.PIPE, text=True,
                                  env=env)
        first = self.p.stderr.readline()
        if "listening" not in first:
            raise RuntimeError(first)

    def stop(self) -> str:
        self.p.terminate()
        err = self.p.communicate(timeout=20)[1]
        return err


def diag(r: Raw) -> dict:
    atom = r.intern("_WOODLAND_DIAG")
    text = r.get_property(ROOT, atom, length=4096).decode()
    out = {}
    for line in text.splitlines():
        k, _, v = line.partition("=")
        out[k] = int(v) if v.lstrip("-").isdigit() else v
    return out


def others_conn_bytes(d: dict) -> int:
    """Connection buffers of everything but the monitor (slot 0, the first connection), whose
    own buffers grow to carry the diagnostics replies."""
    m = re.search(r"in=\d+/(\d+) queue=\d+/(\d+)", d["net.conn.0"])
    return d["net.conn_bytes"] - int(m.group(1)) - int(m.group(2))


def create_pixmap(r: Raw, pid: int, w: int = 16, h: int = 16) -> None:
    r.send(53, 16, struct.pack("<IIHH", pid, ROOT, w, h))


@unittest.skipUnless(os.path.exists(SERVER), "make build/x11_host_server_san first")
class ScaleTest(unittest.TestCase):
    def start(self, *args) -> Server:
        self.srv = Server(*args)
        return self.srv

    def tearDown(self):
        err = self.srv.stop()
        self.assertNotIn("AddressSanitizer", err)
        self.assertNotIn("runtime error", err)  # UBSan
        self.assertEqual(self.srv.p.returncode in (0, -15), True, err[-2000:])

    def baseline_ok(self, mon: Raw, base: dict, what: str):
        """Everything but the monitor connection itself is back to where it was."""
        time.sleep(0.3)
        d = diag(mon)
        for k in ("net.open", "net.core_used", "core.resources", "core.clients_open", "net.reserved"):
            self.assertEqual(d[k], base[k], f"{what}: {k} {base[k]} -> {d[k]}")
        self.assertEqual(others_conn_bytes(d), others_conn_bytes(base), f"{what}: connection buffers")

    def test_128_clients_ids_and_isolation(self):
        srv = self.start("--clients", "129")
        mon = Raw(srv.port)
        base = diag(mon)
        clients = [Raw(srv.port) for _ in range(128)]
        bases = sorted(c.base for c in clients)
        self.assertEqual(len(set(bases)), 128)
        for a, b in zip(bases, bases[1:]):
            self.assertGreaterEqual(b - a, ID_UNIT)  # disjoint ranges
        self.assertLess(bases[-1] + ID_UNIT, 0x20000000)
        for c in clients:  # each creates a window in its own range
            c.create_window(ROOT, 0, 0, 10, 10)
        other = clients[1].base + 50  # an ID in someone else's range
        clients[0].send(1, 0, struct.pack("<IIhhHHHHII", other, ROOT, 0, 0, 5, 5, 0, 1, 0, 0))
        clients[0].sync()
        self.assertEqual([e[1] for e in clients[0].errors], [14])  # BadIDChoice
        for c in clients[1:]:
            c.sync()
            self.assertEqual(c.errors, [])
        d = diag(mon)
        self.assertEqual(d["core.clients_active"], 129)
        self.assertEqual(d["core.resources"], 128)
        for c in clients:
            c.close()
        self.baseline_ok(mon, base, "after 128 clients")

    def test_64_idle_clients_fit_the_four_mb_budget(self):
        # A slot guarantees only its initial input/output buffers. Larger buffers and core
        # resources are charged as allocated, so idle sessions do not reserve 96 KiB each.
        srv = self.start("--clients", "65", "--budget-mb", "4")
        mon = Raw(srv.port)
        base = diag(mon)
        clients = [Raw(srv.port) for _ in range(64)]
        d = diag(mon)
        self.assertEqual(d["core.clients_active"], 65)  # 64 + diagnostics monitor
        self.assertEqual(d["net.open"], 65)
        self.assertEqual(d["net.reserved"], 65 * 1280)
        self.assertEqual(d["net.refused_memory"] - base["net.refused_memory"], 0)
        self.assertEqual(d["net.conn_alloc_failed"] - base["net.conn_alloc_failed"], 0)
        for c in clients:
            c.close()
        self.baseline_ok(mon, base, "after 64 idle clients")

    def test_connection_buffers_are_charged_when_they_grow(self):
        srv = self.start("--clients", "2", "--budget-mb", "4")
        mon = Raw(srv.port)
        base = diag(mon)
        client = Raw(srv.port)
        before = diag(mon)
        # A maximum-sized core request grows the input buffer; unknown opcode 200 gets BadRequest.
        client.send(200, 0, bytes(16380))
        client.sync()
        self.assertEqual([e[1] for e in client.errors], [1])
        after = diag(mon)
        self.assertGreaterEqual(after["net.conn_bytes"] - before["net.conn_bytes"], 14 * 1024)
        self.assertEqual(after["net.reserved"], before["net.reserved"])
        self.assertEqual(after["net.conn_alloc_failed"], before["net.conn_alloc_failed"])
        client.close()
        self.baseline_ok(mon, base, "after grown connection buffer")

    def test_refusal_past_the_limit_is_graceful(self):
        srv = self.start("--clients", "4")
        keep = [Raw(srv.port) for _ in range(4)]
        s, rep = connect(srv.port)
        self.assertEqual(rep["status"], "failed")
        self.assertEqual(rep["reason"], "too many clients")
        s.close()
        for c in keep:
            c.sync()  # the admitted clients are unaffected
        keep[0].close()
        time.sleep(0.3)
        s, rep = connect(srv.port)  # a slot is free again
        self.assertEqual(rep["status"], "success")
        s.close()
        for c in keep[1:]:
            c.close()

    def test_connection_burst(self):
        srv = self.start("--clients", "33")
        mon = Raw(srv.port)
        base = diag(mon)
        socks = []
        for _ in range(100):  # all at once, setup sent immediately
            s = socket.create_connection(("127.0.0.1", srv.port), timeout=10)
            s.sendall(struct.pack("<BxHHHHxx", ord("l"), 11, 0, 0, 0))
            socks.append(s)
        ok = refused = closed = 0
        for s in socks:
            try:
                head = recv_exact(s, 8)
                body = recv_exact(s, 4 * struct.unpack("<H", head[6:8])[0])
                if head[0] == 1:
                    ok += 1
                else:
                    refused += 1
                    self.assertEqual(body[:head[1]], b"too many clients")
            except (ConnectionError, OSError):
                closed += 1  # refused without a reply: the refusal slots were busy
        self.assertEqual(ok, 32)
        self.assertEqual(ok + refused + closed, 100)
        self.assertGreater(refused, 0)
        d = diag(mon)
        self.assertEqual(d["net.refused_slots"] - base["net.refused_slots"], 68)
        for s in socks:
            s.close()
        self.baseline_ok(mon, base, "after the burst")

    def test_slow_readers_do_not_stall_others(self):
        srv = self.start("--clients", "16", "--outq-kb", "128")
        mon = Raw(srv.port)
        base = diag(mon)
        slow = []
        for _ in range(8):  # select PropertyChange on the root, then never read
            r = Raw(srv.port)
            r.select(ROOT, 0x400000)
            r.sync()
            r.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            slow.append(r)
        busy = Raw(srv.port)
        prop = struct.pack("<BBHIIIBxxxI", 18, 0, 7, ROOT, 39, 31, 8, 4) + b"tick"
        worst = 0.0
        for _ in range(60):  # 60,000 events (1.9 MB) per slow reader: more than socket buffers hold
            busy.sock.sendall(prop * 1000)
            busy.seq = (busy.seq + 1000) & 0xFFFF  # sent around the helper
            t = time.perf_counter()
            busy.get_input_focus()
            worst = max(worst, time.perf_counter() - t)
        self.assertLess(worst, 0.5)
        time.sleep(0.3)
        d = diag(mon)
        self.assertEqual(d["net.closed_overflow"] - base["net.closed_overflow"], 8)
        busy.send(19, 0, struct.pack("<II", ROOT, 39))  # DeleteProperty WM_NAME (root properties stay)
        busy.sync()
        busy.close()
        for r in slow:
            r.sock.close()
        self.baseline_ok(mon, base, "after slow readers")

    def test_disconnect_reconnect_cycles(self):
        srv = self.start("--clients", "8")
        mon = Raw(srv.port)
        base = diag(mon)
        for i in range(300):
            r = Raw(srv.port)
            w = r.create_window(ROOT, i % 700, 0, 20, 20)
            create_pixmap(r, r.new_id())
            r.map(w)
            if i % 3 == 0:
                r.sync()
            r.close()
        time.sleep(0.5)
        d = diag(mon)
        self.assertEqual(d["net.accepted"] - base["net.accepted"], 300)
        self.baseline_ok(mon, base, "after 300 cycles")

    def test_resource_quota(self):
        srv = self.start("--clients", "4", "--client-resources", "100")
        mon = Raw(srv.port)
        base = diag(mon)
        hog, other = Raw(srv.port), Raw(srv.port)
        for _ in range(150):
            create_pixmap(hog, hog.new_id())
        hog.sync()
        self.assertEqual(len(hog.errors), 50)
        self.assertTrue(all(e[1] == 11 for e in hog.errors))  # BadAlloc
        create_pixmap(other, other.new_id())
        other.sync()
        self.assertEqual(other.errors, [])  # the other client is not affected
        hog.close()
        other.close()
        self.baseline_ok(mon, base, "after the quota test")

    def test_budget_exhaustion_and_recovery(self):
        # The core uses most of the budget; pixmaps exhaust it and a new slot is refused.
        srv = self.start("--clients", "8", "--budget-mb", "4")
        mon = Raw(srv.port)
        base = diag(mon)
        hog = Raw(srv.port)
        made = []
        for _ in range(40):  # 256x256x2 = 128 KB each until BadAlloc
            pid = hog.new_id()
            create_pixmap(hog, pid, 256, 256)
            made.append(pid)
        hog.sync()
        self.assertGreater(len(hog.errors), 0)
        self.assertTrue(all(e[1] == 11 for e in hog.errors))
        # The connection only reserves its small initial buffers; a 96 KiB queue is not charged
        # until the client actually needs it. A lightweight client can still be admitted here.
        s, rep = connect(srv.port)
        self.assertEqual(rep["status"], "success")
        s.close()
        mon.sync()  # admitted clients still work
        hog.close()  # frees the pixmaps
        time.sleep(0.3)
        s, rep = connect(srv.port)
        self.assertEqual(rep["status"], "success")
        s.close()
        d = diag(mon)
        self.assertGreater(d["net.core_alloc_failed"], 0)
        self.assertEqual(d["net.refused_memory"] - base["net.refused_memory"], 0)
        self.baseline_ok(mon, base, "after exhaustion")

    def test_reset_with_pending_events(self):
        srv = self.start("--clients", "8")
        mon = Raw(srv.port)
        base = diag(mon)
        victims = []
        for _ in range(4):
            r = Raw(srv.port)
            r.select(ROOT, 0x400000)
            r.sync()
            victims.append(r)
        busy = Raw(srv.port)
        for _ in range(20):
            busy.sock.sendall((struct.pack("<BBHIIIBxxxI", 18, 0, 7, ROOT, 39, 31, 8, 4) + b"tick") * 200)
            busy.seq = (busy.seq + 200) & 0xFFFF
        for r in victims:  # abortive close (RST) while events are queued
            r.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            r.sock.close()
        busy.get_input_focus()
        busy.send(19, 0, struct.pack("<II", ROOT, 39))  # DeleteProperty WM_NAME (root properties stay)
        busy.sync()
        busy.close()
        self.baseline_ok(mon, base, "after resets")


if __name__ == "__main__":
    unittest.main()
