"""Trace metrics are bounded, emitted through diagnostics, and survive client cleanup."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(HERE, "build", "x11_host_server_trace")
BENCH = os.path.join(HERE, "tools", "x11_bench.py")


@unittest.skipUnless(os.path.exists(SERVER), "make build/x11_host_server_trace first")
class LatencyTraceTest(unittest.TestCase):
    def test_graphics_trace_and_queues_after_client_cleanup(self):
        display = 610 + os.getpid() % 150
        server = subprocess.Popen(
            [SERVER, "-d", str(display), "-q", "--clients", "8", "--budget-mb", "8"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        try:
            self.assertIn("listening", server.stderr.readline())
            with tempfile.NamedTemporaryFile(mode="r+", suffix=".json") as result:
                subprocess.run(
                    [sys.executable, BENCH, "--display", f"127.0.0.1:{display}", "--no-auth",
                     "--levels", "3", "--workload", "graphics", "--hold", "0.8", "--rate", "10",
                     "--diag-every", "0.05", "--diag-timeout", "1", "--json", result.name],
                    check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=15,
                )
                result.seek(0)
                row = json.load(result)["results"][0]
            trace = row["diag_peak"]
            self.assertEqual(row["button_events_lost"], 0)
            self.assertTrue(row["reply_order_ok"])
            self.assertEqual(row["cleanup_delta"]["net.queue_bytes"], 0)
            for name in ("select_ready_dispatch", "request_wait", "input_age", "dispatch", "diag_dispatch",
                         "output_first", "output_drain", "output_flush", "client_visit", "net_pass",
                         "polyfill"):
                self.assertGreater(int(trace[f"net.trace.{name}.n"]), 0, name)
                self.assertGreaterEqual(int(trace[f"net.trace.{name}.p99_us"]), 0, name)
                self.assertGreaterEqual(int(trace[f"net.trace.{name}.max_us"]),
                                        int(trace[f"net.trace.{name}.p50_us"]), name)
            self.assertGreater(int(trace["net.trace.polyfill_requests"]), 0)
            self.assertGreater(int(trace["net.trace.polyfill_rectangles"]), 0)
            self.assertGreater(int(trace["net.trace.polyfill_clipped_pixels"]), 0)
            self.assertGreater(int(trace["net.trace.input_depth_max"]), 0)
            self.assertGreater(int(trace["net.trace.output_depth_max"]), 0)
            self.assertGreater(int(trace["net.trace.static_bytes"]), 0)
        finally:
            server.terminate()
            _, err = server.communicate(timeout=10)
            self.assertNotIn("AddressSanitizer", err)
            self.assertNotIn("runtime error:", err)


if __name__ == "__main__":
    unittest.main()
