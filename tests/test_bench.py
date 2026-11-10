"""The scaling harness uses real X11 setup, drawing, events, replies, and cleanup."""
from __future__ import annotations

import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(HERE, "build", "x11_host_server_san")
BENCH = os.path.join(HERE, "tools", "x11_bench.py")


@unittest.skipUnless(os.path.exists(SERVER), "make build/x11_host_server_san first")
class BenchTest(unittest.TestCase):
    def test_graphics_workload_delivers_events_and_reclaims_resources(self):
        display = 400 + os.getpid() % 300
        port = 6000 + display
        server = subprocess.Popen(
            [SERVER, "-d", str(display), "-q", "--clients", "4", "--budget-mb", "4",
             "--outq-kb", "96", "--resources", "1024"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            first = server.stderr.readline()
            self.assertIn("listening", first)
            with tempfile.NamedTemporaryFile(mode="r+", suffix=".json") as result:
                subprocess.run(
                    [sys.executable, BENCH, "--display", f"127.0.0.1:{display}", "--no-auth",
                     "--levels", "2", "--workload", "graphics", "--hold", "0.5",
                     "--rate", "4", "--diag-every", "0.1", "--json", result.name],
                    check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=15)
                result.seek(0)
                row = json.load(result)["results"][0]
            self.assertEqual(row["x11_set_up"], 2)
            self.assertEqual(row["active_whole_hold"], 2)
            self.assertGreater(row["button_events_sent"], 0)
            self.assertEqual(row["button_events_received"], row["button_events_sent"])
            self.assertEqual(row["button_events_lost"], 0)
            self.assertEqual(row["x11_errors"], {})
            self.assertTrue(all(v == 0 for v in row["cleanup_delta"].values()))
            self.assertGreater(row["diag_peak"]["net.graphics_requests"], 0)
            self.assertGreater(row["diag_delta"]["net.graphics_requests"], 0)
            self.assertIn("net.request_buffer_wait_us_max", row["diag_peak"])
            self.assertIn("net.diag_service_us_max", row["diag_peak"])
            self.assertIn("net.passes_exceeded_request_budget", row["diag_peak"])
            self.assertTrue(row["net_client_pass"])
            self.assertTrue(all("pass_reqs_max" in v for v in row["net_client_pass"].values()))
        finally:
            server.terminate()
            _, err = server.communicate(timeout=15)
            self.assertNotIn("AddressSanitizer", err)
            self.assertNotIn("runtime error:", err)

    def test_diagnostics_latency_threshold_stops_without_waiting_for_timeout(self):
        display = 500 + os.getpid() % 200
        server = subprocess.Popen(
            [SERVER, "-d", str(display), "-q", "--clients", "8", "--budget-mb", "4",
             "--outq-kb", "96", "--resources", "1024", "--request-budget-us", "6000"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            first = server.stderr.readline()
            self.assertIn("listening", first)
            with tempfile.NamedTemporaryFile(mode="r+", suffix=".json") as result:
                subprocess.run(
                    [sys.executable, BENCH, "--display", f"127.0.0.1:{display}", "--no-auth",
                     "--levels", "2", "--workload", "graphics", "--hold", "5",
                     "--rate", "4", "--diag-every", "0.1", "--diag-timeout", "1",
                     "--max-diag-ms", "0.0001", "--json", result.name],
                    check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=15)
                result.seek(0)
                row = json.load(result)["results"][0]
            self.assertIn("diagnostics response", row["stopped"])
            self.assertLess(row["hold_s"], 1.0)
            self.assertGreater(row["diag_rtt_ms_max"], 0.0001)
        finally:
            server.terminate()
            _, err = server.communicate(timeout=15)
            self.assertNotIn("AddressSanitizer", err)
            self.assertNotIn("runtime error:", err)

    def test_extra_probe_reports_slot_refusal_without_dropping_clients(self):
        display = 700 + os.getpid() % 100
        server = subprocess.Popen(
            [SERVER, "-d", str(display), "-q", "--clients", "4", "--budget-mb", "4",
             "--outq-kb", "96", "--resources", "1024"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        background = None
        try:
            first = server.stderr.readline()
            self.assertIn("listening", first)
            background = socket.create_connection(("127.0.0.1", 6000 + display), timeout=5)
            background.sendall(struct.pack("<BxHHHHxx", ord("l"), 11, 0, 0, 0))
            header = background.recv(8)
            self.assertEqual(len(header), 8)
            setup_size = 8 + 4 * struct.unpack_from("<H", header, 6)[0]
            while len(header) < setup_size:
                header += background.recv(setup_size - len(header))
            self.assertEqual(header[0], 1)
            with tempfile.NamedTemporaryFile(mode="r+", suffix=".json") as result:
                subprocess.run(
                    [sys.executable, BENCH, "--display", f"127.0.0.1:{display}", "--no-auth",
                     "--levels", "2", "--workload", "idle", "--hold", "0.3",
                     "--diag-every", "0.1", "--probe-extra", "--background-clients", "1",
                     "--json", result.name],
                    check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                    text=True, timeout=15)
                result.seek(0)
                row = json.load(result)["results"][0]
            self.assertEqual(row["x11_set_up"], 2)
            self.assertEqual(row["active_whole_hold"], 2)
            self.assertEqual(row["x11_clients_total"], 4)
            self.assertEqual(row["extra_probe"]["state"], "failed")
            self.assertIn("setup refused", row["extra_probe"]["reason"])
            self.assertIn("too many clients", row["extra_probe"]["reason"])
            self.assertEqual(row["lost_during_hold"], 0)
            self.assertTrue(all(v == 0 for v in row["cleanup_delta"].values()))
        finally:
            if background:
                background.close()
            server.terminate()
            _, err = server.communicate(timeout=15)
            self.assertNotIn("AddressSanitizer", err)
            self.assertNotIn("runtime error:", err)


if __name__ == "__main__":
    unittest.main()
