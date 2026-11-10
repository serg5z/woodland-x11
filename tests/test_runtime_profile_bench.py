"""The stress report preserves bounded dual-core profiler fields, not peer strings."""
from __future__ import annotations

import os
import json
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools")
sys.path.insert(0, TOOLS)
import x11_bench  # noqa: E402
import runtime_profile_sample  # noqa: E402

SERVER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "x11_host_server_san")
BENCH = os.path.join(TOOLS, "x11_bench.py")


class RuntimeProfileBenchTest(unittest.TestCase):
    def test_diag_peak_keeps_runtime_fields_and_drops_connection_details(self):
        peak = {
            "rt.core0_busy_pct": "2.01",
            "rt.task.loopTask.cpu_pct": "22.73",
            "rt.task.loopTask.stack_free_bytes": "5148",
            "net.request_us_max": 43714,
            "net.conn.0": "peer=192.0.2.1:1234 requests=1",
        }
        got = x11_bench.diag_peak_fields(peak)
        self.assertEqual(got["rt.core0_busy_pct"], "2.01")
        self.assertEqual(got["rt.task.loopTask.cpu_pct"], "22.73")
        self.assertEqual(got["rt.task.loopTask.stack_free_bytes"], "5148")
        self.assertEqual(got["net.request_us_max"], 43714)
        self.assertNotIn("net.conn.0", got)

    def test_runtime_samples_keep_decimal_core_and_task_percentages(self):
        got = x11_bench.runtime_sample_fields({
            "rt.core0_busy_pct": "2.01",
            "rt.core1_busy_pct": "24.63",
            "rt.task.loopTask.cpu_pct": "22.73",
            "net.conn.0": "peer=192.0.2.1:1234 requests=1",
        })
        self.assertEqual(got["rt.core0_busy_pct"], "2.01")
        self.assertEqual(got["rt.core1_busy_pct"], "24.63")
        self.assertEqual(got["rt.task.loopTask.cpu_pct"], "22.73")
        self.assertNotIn("net.conn.0", got)

    def test_static_sample_parser_decodes_xprop_and_redacts_peer_fields(self):
        escaped_newline = chr(92) + "n"
        text = ('_WOODLAND_DIAG(STRING) = "esp.uptime_ms=123' + escaped_newline +
                'rt.core0_busy_pct=2.01' + escaped_newline + 'net.conn.0=peer=192.0.2.1:1234"')
        diag = runtime_profile_sample.parse_xprop(text)
        self.assertEqual(diag["esp.uptime_ms"], "123")
        self.assertEqual(diag["rt.core0_busy_pct"], "2.01")
        self.assertNotIn("net.conn.0", runtime_profile_sample.selected(diag))

    @unittest.skipUnless(os.path.exists(SERVER), "make build/x11_host_server_san first")
    def test_graphics_boundary_drains_inflight_events_before_close(self):
        display = 450 + os.getpid() % 200
        server = subprocess.Popen(
            [SERVER, "-d", str(display), "-q", "--clients", "4", "--budget-mb", "4"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        try:
            self.assertIn("listening", server.stderr.readline())
            with tempfile.NamedTemporaryFile(mode="r+", suffix=".json") as result:
                subprocess.run(
                    [sys.executable, BENCH, "--display", f"127.0.0.1:{display}", "--no-auth",
                     "--levels", "1", "--workload", "graphics", "--hold", "0.5", "--rate", "20",
                     "--diag-every", "0.05", "--diag-timeout", "1", "--json", result.name],
                    check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=15,
                )
                result.seek(0)
                row = json.load(result)["results"][0]
            self.assertGreater(row["button_events_sent"], 0)
            self.assertEqual(row["button_events_received"], row["button_events_sent"])
            self.assertEqual(row["pending_events_after_drain"], 0)
            self.assertLessEqual(row["event_drain_s"], 0.5)
        finally:
            server.terminate()
            _, err = server.communicate(timeout=15)
            self.assertNotIn("AddressSanitizer", err)
            self.assertNotIn("runtime error:", err)

    @unittest.skipUnless(os.path.exists(SERVER), "make build/x11_host_server_san first")
    def test_reply_rtt_gate_stops_protocol_work(self):
        display = 520 + os.getpid() % 180
        server = subprocess.Popen(
            [SERVER, "-d", str(display), "-q", "--clients", "4", "--budget-mb", "4"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        try:
            self.assertIn("listening", server.stderr.readline())
            with tempfile.NamedTemporaryFile(mode="r+", suffix=".json") as result:
                subprocess.run(
                    [sys.executable, BENCH, "--display", f"127.0.0.1:{display}", "--no-auth",
                     "--levels", "1", "--workload", "protocol", "--hold", "2", "--rate", "4",
                     "--diag-every", "0.05", "--diag-timeout", "1", "--max-rtt-ms", "0.000001",
                     "--json", result.name],
                    check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=15,
                )
                result.seek(0)
                row = json.load(result)["results"][0]
            self.assertIn("X11 reply RTT", row["stopped"])
            self.assertGreater(row["rtt_ms_p99"], 0.000001)
        finally:
            server.terminate()
            _, err = server.communicate(timeout=15)
            self.assertNotIn("AddressSanitizer", err)
            self.assertNotIn("runtime error:", err)


if __name__ == "__main__":
    unittest.main()
