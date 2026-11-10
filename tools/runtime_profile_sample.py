#!/usr/bin/env python3
"""Collect bounded _WOODLAND_DIAG runtime samples without retaining peer addresses."""
from __future__ import annotations

import argparse
import json
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path


def parse_xprop(text: str) -> dict[str, str]:
    value = text.split(" = ", 1)[-1].strip().strip('"')
    value = value.replace(r"\n", "\n").replace(r'\"', '"').replace(r"\\", "\\")
    result = {}
    for line in value.splitlines():
        key, sep, val = line.partition("=")
        if sep:
            result[key] = val
    return result


def selected(diag: dict[str, str]) -> dict[str, str]:
    prefixes = (
        "esp.uptime_ms", "esp.reset_reason", "esp.boot_count", "esp.heap_internal_",
        "esp.psram_", "esp.loop_stack_free", "esp.wifi_losses", "esp.clients_max",
        "lcd.ok", "lcd.flush_us_max_10s", "lcd.pending_rows", "touch.on", "touch.polls",
        "touch.interval_max_10s_ms", "touch.late", "touch.presses", "touch.releases",
        "ota.running", "ota.running_state", "ota.other", "ota.other_state", "ota.health",
        "spi.x11.active", "spi.bad_crc", "spi.retransmits", "spi.resyncs", "spi.queued_out",
        "net.errors", "net.passes_over_budget", "net.passes_exceeded_request_budget",
        "net.request_us_max", "net.request_us_max_op", "net.pass_us_max", "net.net_us_max",
        "net.request_budget_over_us_max", "net.diag_buffer_wait_us_max", "net.diag_service_us_max",
        "net.write_would_block", "net.write_partial", "net.heap_internal_free",
        "rt.",
    )
    return {k: v for k, v in diag.items() if any(k == p or k.startswith(p) for p in prefixes)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--display", required=True)
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--interval", type=float, default=1)
    ap.add_argument("--timeout", type=float, default=1.5)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.seconds <= 0 or a.interval <= 0 or a.timeout <= 0:
        ap.error("seconds, interval and timeout must be positive")
    path = Path(a.out)
    path.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    count = 0
    with path.open("w", encoding="utf-8") as out:
        while time.monotonic() - start < a.seconds:
            sample_start = time.monotonic()
            try:
                proc = subprocess.run(
                    ["xprop", "-display", a.display, "-root", "_WOODLAND_DIAG"],
                    capture_output=True, text=True, timeout=a.timeout, check=True,
                )
                diag = parse_xprop(proc.stdout)
            except (subprocess.SubprocessError, OSError) as exc:
                print(f"sample {count + 1} failed: {type(exc).__name__}: {exc}")
                return 2
            record = {
                "utc": datetime.now(timezone.utc).isoformat(),
                "elapsed_s": round(sample_start - start, 3),
                "diag": selected(diag),
            }
            out.write(json.dumps(record, sort_keys=True) + "\n")
            out.flush()
            count += 1
            remaining = a.interval - (time.monotonic() - sample_start)
            if remaining > 0:
                time.sleep(min(remaining, max(0, a.seconds - (time.monotonic() - start))))
    print(f"samples={count} elapsed_s={time.monotonic() - start:.1f} output={path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
