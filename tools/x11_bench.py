#!/usr/bin/env python3
"""Client scalability benchmark for the Woodland X11 server (host server or ESP32).

Opens N authenticated X11 clients at once from one nonblocking event loop, runs a workload
for a while, reads the server's own counters from the root property _WOODLAND_DIAG through a
separate monitor connection, then closes everything and checks that the server returned to
its baseline (no connection, resource or memory left behind).

    tools/x11_bench.py --display 127.0.0.1:177 --no-auth --levels 4,8,16 --workload all --hold 60
    tools/x11_bench.py --display PANEL_IP:0 --levels 2,4 --workload protocol --hold 60 \\
        --stop 'esp.heap_internal_free<60000' --stop 'touch.interval_max_10s_ms>60'

Workloads:
  idle      authenticated connections that send nothing
  protocol  each client: a request with a reply (GetInputFocus, InternAtom only-if-exists,
            QueryPointer, in turn) --rate times a second; round trip per request
  graphics  each client: one mapped 48x48 window and a GC; --rate times a second a
            PolyFillRectangle in a new colour followed by GetInputFocus (round trip = drawn)
  fullscreen  as graphics, but the clients share the screen below y=60 in horizontal bands
            (one client: 800x420), each filled whole every time: the most pixels per request,
            for display/PSRAM load tests; the top 60 px stay untouched
  copy      CopyArea root-to-root over 799x480 pixels, then GetInputFocus; useful for scheduler
            and same-framebuffer copy regressions (use host --copy-reference for a slow baseline)

Levels are clients doing the workload; the monitor is one more connection (the server must
allow levels+1 clients). Three things are reported separately: TCP connections that the
server accepted, clients that completed X11 setup (authenticated), and clients that did
their work without errors for the whole hold.

--stop KEY<VALUE / KEY>VALUE: stop thresholds checked on every diagnostics read (every
--diag-every seconds); when one trips, all clients are closed at once and the run ends
with "stopped". Use them on hardware.

The cookie (unless --no-auth) is read from `xauth list` into memory and sent only in the
connection setup; it is never printed or written anywhere. Results: --json FILE and a
table on stdout. Existing X11 clients outside the benchmark can be counted with
--background-clients; they are neither created nor controlled by the benchmark."""
from __future__ import annotations

import argparse
import errno
import json
import os
import re
import selectors
import socket
import struct
import subprocess
import sys
import time

ROOT = 0x26
DIAG = "_WOODLAND_DIAG"


def percentile(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    k = min(len(xs) - 1, max(0, int(round(p / 100.0 * (len(xs) - 1)))))
    return xs[k]


def diag_peak_fields(peak):
    """Keep bounded counters and profile metrics; omit per-peer diagnostic strings."""
    return {k: v for k, v in peak.items() if k.startswith(("esp.", "lcd.", "touch.", "host.", "rt.", "net.trace.")) or
            k in ("net.open", "net.accepted", "net.refused_slots", "net.refused_memory",
                  "net.setup_ok", "net.setup_failed", "net.setup_timeouts", "core.clients_active",
                  "core.clients_open", "net.core_alloc_failed", "net.conn_alloc_failed",
                  "net.requests_pass_max", "net.client_requests_pass_max", "net.request_us_max",
                  "net.request_us_max_op", "net.pass_us_max", "net.graphics_requests",
                  "net.graphics_us_total", "net.graphics_us_max", "net.write_would_block",
                  "net.write_partial", "net.write_stall_queue_max", "net.backpressure_waits",
                  "net.passes_over_budget", "net.passes_exceeded_request_budget",
                  "net.request_budget_over_us_max", "net.request_buffer_wait_us_max",
                  "net.diag_buffer_wait_us_max", "net.diag_service_us_max",
                  "net.output_queue_age_us_max", "net.output_queue_age_us_total",
                  "net.output_queue_drains", "net.readable_dispatch_us_max",
                  "net.readable_dispatch_us_total", "net.readable_dispatch_samples")}


def runtime_sample_fields(diag):
    """Preserve the profile's per-sample decimal percentages for workload analysis."""
    return {k: v for k, v in diag.items() if k.startswith("rt.")}


def cookie_for(display, authority_file=None):
    command = ["xauth"]
    if authority_file:
        command += ["-f", authority_file]
    out = subprocess.run(command + ["list", display], capture_output=True, text=True, check=False).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] == "MIT-MAGIC-COOKIE-1":
            return bytes.fromhex(parts[2])
    raise SystemExit(f"no MIT-MAGIC-COOKIE-1 for {display} in xauth")


def setup_request(auth_name, auth_data):
    pad = lambda b: b + b"\0" * (-len(b) % 4)
    return struct.pack("<BxHHHHxx", ord("l"), 11, 0, len(auth_name), len(auth_data)) + pad(auth_name) + pad(auth_data)


def req(op, data1=0, body=b""):
    return struct.pack("<BBH", op, data1, (4 + len(body)) // 4) + body


def monitor_bytes(d, slot):
    """The monitor connection's own buffers (they grow to carry the diagnostics replies)."""
    if slot is None:
        return 0
    m = re.search(r"in=\d+/(\d+) queue=\d+/(\d+)", str(d.get(f"net.conn.{slot}", "")))
    return int(m.group(1)) + int(m.group(2)) if m else 0


def monitor_conn_slot(d, local_port):
    """Find the diagnostic connection by its TCP source port, even with other clients open."""
    for key, value in d.items():
        m = re.fullmatch(r"net\.conn\.(\d+)", key)
        if m and re.search(rf"peer=[^ ]*:{local_port}(?:\s|$)", str(value)):
            return int(m.group(1))
    return None


def parse_diag(text):
    d = {}
    for line in text.splitlines():
        k, sep, v = line.partition("=")
        if not sep:
            continue
        try:
            d[k] = int(v)
        except ValueError:
            d[k] = v
    return d


class Client:
    """One X11 connection, LSB first, driven by the shared selector."""

    def __init__(self, bench, idx):
        self.b, self.idx = bench, idx
        self.sock = None
        self.state = "new"          # connecting, setup, ready, closed, failed
        self.buf = b""
        self.seq = 0
        self.pending = {}           # seq -> (send time, kind)
        self.base = 0
        self.reason = ""
        self.t_connect = self.t_setup = None
        self.errors = 0
        self.events = 0
        self.replies = 0
        self.out = b""
        self.event_pending = {}
        self.event_lat = []
        self.rtt_lat = []
        self.reply_order_sent = []
        self.reply_order_received = []
        self.events_sent = 0
        self.next_at = 0.0
        self.turn = 0
        self.win = self.gc = 0
        self.diag_cb = None

    # -- wire --
    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.setblocking(False)
        self.t_connect = time.perf_counter()
        try:
            self.sock.connect((self.b.host, self.b.port))
        except BlockingIOError:
            pass
        except OSError as e:
            return self.fail(f"connect: {e.strerror}")
        self.state = "connecting"
        self.b.sel.register(self.sock, selectors.EVENT_READ | selectors.EVENT_WRITE, self)

    def fail(self, why):
        if self.state in ("closed", "failed"):
            return
        prior = self.state
        self.state = "failed"
        self.reason = why
        self.failed_in = prior
        self.close_socket()

    def close_socket(self):
        if self.sock:
            try:
                self.b.sel.unregister(self.sock)
            except (KeyError, ValueError):
                pass
            self.sock.close()
            self.sock = None

    def close(self):
        if self.state not in ("failed",):
            self.state = "closed"
        self.close_socket()

    def send(self, data):
        self.out += data
        self.flush()

    def flush(self):
        if not self.out or not self.sock:
            return
        try:
            n = self.sock.send(self.out)
            self.out = self.out[n:]
        except BlockingIOError:
            pass
        except OSError as e:
            self.fail(f"send: {e.strerror}")
            return
        if self.sock:
            self.b.sel.modify(self.sock, selectors.EVENT_READ | (selectors.EVENT_WRITE if self.out else 0), self)

    def request(self, data, kind=None, reply=False):
        self.seq = (self.seq + 1) & 0xFFFF
        if reply:
            self.pending[self.seq] = (time.perf_counter(), kind)
            self.reply_order_sent.append(self.seq)
        self.send(data)
        return self.seq

    # -- events from the selector --
    def on_write(self):
        if self.state == "connecting":
            err = self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            if err:
                return self.fail(f"connect: {os.strerror(err)}")
            self.state = "setup"
            self.send(setup_request(*self.b.auth))
        else:
            self.flush()

    def on_read(self):
        try:
            data = self.sock.recv(65536)
        except BlockingIOError:
            return
        except OSError as e:
            return self.fail(f"recv: {e.strerror or e}")
        if not data:
            return self.fail("closed by server" if self.state != "setup" else "closed during setup")
        self.buf += data
        while self.sock:
            if self.state == "setup":
                if len(self.buf) < 8:
                    return
                n = 8 + 4 * struct.unpack_from("<H", self.buf, 6)[0]
                if len(self.buf) < n:
                    return
                m, self.buf = self.buf[:n], self.buf[n:]
                self.t_setup = time.perf_counter()
                if m[0] != 1:
                    return self.fail("setup refused: " + m[8:8 + m[1]].decode(errors="replace"))
                self.base = struct.unpack_from("<I", m, 12)[0]
                self.state = "ready"
                self.b.on_ready(self)
                continue
            if len(self.buf) < 32:
                return
            n = 32 + (4 * struct.unpack_from("<I", self.buf, 4)[0] if self.buf[0] == 1 else 0)
            if len(self.buf) < n:
                return
            m, self.buf = self.buf[:n], self.buf[n:]
            if m[0] == 1:
                seq = struct.unpack_from("<H", m, 2)[0]
                self.reply_order_received.append(seq)
                sent = self.pending.pop(seq, None)
                self.replies += 1
                if sent:
                    self.b.on_reply(self, sent[1], time.perf_counter() - sent[0], m)
            elif m[0] == 0:
                self.errors += 1
                seq = struct.unpack_from("<H", m, 2)[0]
                self.pending.pop(seq, None)
                self.b.on_error(self, m)
            else:
                self.events += 1
                # SendEvent copies the sequence of the request that produced the event.
                # Track only our synthetic ButtonPress events; server touch remains a separate
                # physical measurement on hardware.
                if (m[0] & 0x7F) == 4:
                    seq = struct.unpack_from("<H", m, 2)[0]
                    sent = self.event_pending.pop(seq, None)
                    if sent is not None:
                        self.event_lat.append(time.perf_counter() - sent)


class Bench:
    def __init__(self, a):
        self.a = a
        host, _, num = a.display.rpartition(":")
        self.host = host or "127.0.0.1"
        self.port = 6000 + int(num.split(".")[0])
        self.auth = (b"", b"") if a.no_auth else (b"MIT-MAGIC-COOKIE-1", cookie_for(a.display, a.xauthority))
        self.sel = selectors.DefaultSelector()
        self.stops = []
        for s in a.stop:
            m = re.fullmatch(r"([\w.]+)\s*([<>])\s*(-?\d+)", s)
            if not m:
                raise SystemExit(f"bad --stop {s!r} (KEY<VALUE or KEY>VALUE)")
            self.stops.append((m.group(1), m.group(2), int(m.group(3))))
        self.monitor = None
        self.diag_atom = 0
        self.lat = []
        self.diag_lat = []
        self.last_diag_ms = None
        self.measuring = False
        self.workload = None
        self.clients = []
        self.extra = None
        self.stopped = None

    # -- event loop --
    def run_until(self, cond, timeout):
        end = time.perf_counter() + timeout
        while not cond() and time.perf_counter() < end:
            self.step(min(0.05, max(0.0, end - time.perf_counter())))
        return cond()

    def step(self, timeout):
        for key, ev in self.sel.select(timeout):
            c = key.data
            if ev & selectors.EVENT_WRITE and c.sock:
                c.on_write()
            if ev & selectors.EVENT_READ and c.sock:
                c.on_read()
        now = time.perf_counter()
        if self.workload in ("protocol", "graphics", "fullscreen", "copy"):
            for c in self.clients:
                if c.state == "ready" and c.sock and now >= c.next_at and len(c.pending) < 4 and c.win >= 0:
                    c.next_at = now + 1.0 / self.a.rate
                    self.tick(c)

    # -- callbacks --
    def on_ready(self, c):
        if c is self.monitor:
            body = struct.pack("<HH", len(DIAG), 0) + DIAG.encode() + b"\0" * (-len(DIAG) % 4)
            c.request(req(16, 1, body), "intern", reply=True)  # InternAtom only-if-exists
            return
        if c is self.extra:
            return  # probe only setup/admission; never issue workload requests
        if self.workload == "copy":
            c.win = ROOT  # sentinel: this workload uses the root drawable directly
            c.gc = c.base + 1
            c.request(req(55, 0, struct.pack("<III", c.gc, ROOT, 0)))  # CreateGC
            c.next_at = time.perf_counter() + 1.0 / max(self.a.rate, 0.1)
            return
        if self.workload in ("graphics", "fullscreen"):
            c.win = c.base + 1
            c.gc = c.base + 2
            k = self.clients.index(c)
            if self.workload == "graphics":
                x, y = (k * 52) % 750, ((k * 52) // 750 * 52) % 430
                c.w = c.h = 48
            else:  # bands of the 800x420 area below the static top strip
                n = len(self.clients)
                x, y, c.w = 0, 60 + k * 420 // n, 800
                c.h = 60 + (k + 1) * 420 // n - y
            # CreateWindow(override-redirect, background pixel)
            body = struct.pack("<IIhhHHHHII", c.win, ROOT, x, y, c.w, c.h, 0, 1, 0, (1 << 1) | (1 << 9)) + struct.pack(
                "<II", 0x0000, 1)
            c.request(req(1, 0, body))
            # Exercise event selection and delivery as well as drawing. Exposure/structure
            # events come from normal map/configure operations; ButtonPress is sent below.
            event_mask = (1 << 2) | (1 << 3) | (1 << 15) | (1 << 17)
            c.request(req(2, 0, struct.pack("<III", c.win, 1 << 11, event_mask)))  # ChangeWindowAttributes
            c.request(req(55, 0, struct.pack("<IIII", c.gc, c.win, 1 << 2, 0xFFFF)))  # CreateGC fg
            c.request(req(8, 0, struct.pack("<I", c.win)))  # MapWindow
        c.next_at = time.perf_counter() + (self.clients.index(c) % 10) / (10.0 * max(self.a.rate, 0.1))

    def tick(self, c):
        c.turn += 1
        if self.workload == "protocol":
            k = c.turn % 3
            if k == 0:
                c.request(req(43), "rtt", reply=True)  # GetInputFocus
            elif k == 1:
                c.request(req(16, 1, struct.pack("<HH", 7, 0) + b"WM_NAME\0"), "rtt", reply=True)
            else:
                c.request(req(38, 0, struct.pack("<I", ROOT)), "rtt", reply=True)  # QueryPointer
        elif self.workload == "copy":
            c.request(req(62, 0, struct.pack("<IIIhhhhHH", ROOT, ROOT, c.gc, 0, 0, 1, 0, 799, 480)))
            c.request(req(43), "rtt", reply=True)
        else:
            colour = (c.turn * 2654435761) & 0xFFFF
            c.request(req(56, 0, struct.pack("<III", c.gc, 1 << 2, colour)))  # ChangeGC foreground
            c.request(req(70, 0, struct.pack("<II", c.win, c.gc) + struct.pack("<hhHH", 0, 0, c.w, c.h)))
            event_time = int(time.monotonic() * 1000) & 0xFFFFFFFF
            event = struct.pack("<BBHIIIIhhhhHB", 4, 1, 0, event_time, ROOT, c.win, 0,
                                0, 0, 0, 0, 0, 1) + b"\0"
            seq = c.request(req(25, 0, struct.pack("<II", c.win, 1 << 2) + event))  # SendEvent ButtonPress
            c.event_pending[seq] = time.perf_counter()
            c.events_sent += 1
            c.request(req(43), "rtt", reply=True)

    def on_reply(self, c, kind, dt, m):
        if c is self.monitor:
            if kind == "intern":
                self.diag_atom = struct.unpack_from("<I", m, 8)[0]
            elif kind == "diag" and c.diag_cb:
                self.last_diag_ms = 1000.0 * dt
                if self.measuring:
                    self.diag_lat.append(self.last_diag_ms)
                n = struct.unpack_from("<I", m, 16)[0]
                c.diag_cb(m[32:32 + n].decode(errors="replace"))
            return
        if kind == "rtt" and self.measuring:
            self.lat.append(dt)
            c.rtt_lat.append(dt)
            self.req_count += 1

    def on_error(self, c, m):
        if c is not self.monitor:
            self.error_codes[m[1]] = self.error_codes.get(m[1], 0) + 1

    # -- diagnostics --
    def diag(self, timeout=None):
        got = {}
        mon = self.monitor
        if not mon or mon.state != "ready" or not self.diag_atom:
            return None
        if timeout is None:
            timeout = self.a.diag_timeout
        mon.diag_cb = lambda text: got.update(parse_diag(text) or {"_empty": 1})
        mon.request(req(20, 0, struct.pack("<IIIII", ROOT, self.diag_atom, 0, 0, 16384)), "diag", reply=True)
        self.run_until(lambda: bool(got) or mon.state != "ready", timeout)
        mon.diag_cb = None
        return got or None

    def check_stops(self, d):
        boot = d.get("esp.boot_count")
        if isinstance(boot, int):  # the panel restarted between two reads
            if getattr(self, "boot0", None) is None:
                self.boot0 = boot
            elif boot != self.boot0:
                return f"esp.boot_count {self.boot0} -> {boot} (the panel restarted)"
        for key, op, val in self.stops:
            if key in d and isinstance(d[key], int):
                if (op == "<" and d[key] < val) or (op == ">" and d[key] > val):
                    return f"{key}={d[key]} ({op}{val})"
        return None

    def server_cpu_rss(self):
        if not self.a.server_pid:
            return None, None
        out = subprocess.run(["ps", "-o", "time=,rss=", "-p", str(self.a.server_pid)], capture_output=True,
                             text=True).stdout.split()
        if len(out) < 2:
            return None, None
        sec = 0.0
        for v in out[0].replace("-", ":").split(":"):
            sec = sec * 60 + float(v)
        return sec, int(out[1]) * 1024

    # -- one level --
    def level(self, workload, n):
        self.workload = workload
        self.lat, self.diag_lat, self.req_count, self.error_codes = [], [], 0, {}
        self.last_diag_ms = None
        self.measuring = False
        base = self.diag() or {}
        cpu0, rss0 = self.server_cpu_rss()
        self.clients = [Client(self, i) for i in range(n)]
        t0 = time.perf_counter()
        for c in self.clients:  # a simultaneous burst
            c.start()
        self.run_until(lambda: all(c.state in ("ready", "failed") for c in self.clients), self.a.connect_timeout)
        t_burst = time.perf_counter() - t0
        ready = [c for c in self.clients if c.state == "ready"]
        setup_ms = [1000 * (c.t_setup - c.t_connect) for c in ready]
        if workload in ("graphics", "fullscreen"):  # windows exist before measuring
            self.run_until(lambda: False, 1.0)
        extra_probe = None
        if self.a.probe_extra and n == max(self.a.levels):
            # At this point the diagnostic monitor and all workload clients are still open.
            # This checks the next X11 setup slot, not merely whether TCP accept succeeds.
            self.extra = Client(self, -2)
            self.extra.start()
            self.run_until(lambda: self.extra.state in ("ready", "failed"), self.a.connect_timeout)
            extra_probe = {"state": self.extra.state, "reason": self.extra.reason}
            self.extra.close()
            self.run_until(lambda: False, 0.15)
            self.extra = None
        self.measuring = True
        t_hold = time.perf_counter()
        cpu1, _ = self.server_cpu_rss()
        peak = dict(base)
        samples = []
        rt_samples = []
        while time.perf_counter() - t_hold < self.a.hold and not self.stopped:
            self.run_until(lambda: False, min(self.a.diag_every, self.a.hold - (time.perf_counter() - t_hold)))
            d = self.diag(timeout=self.a.diag_timeout)
            if self.monitor.state != "ready":
                self.stopped = f"monitor connection lost ({self.monitor.reason or 'closed'}): panel reset or network?"
            elif not d:
                self.stopped = f"diagnostics did not answer within {self.a.diag_timeout:g} s"
            if d:
                samples.append(d)
                rt_values = runtime_sample_fields(d)
                if rt_values and len(rt_samples) < 240:
                    rt_samples.append({"elapsed_s": round(time.perf_counter() - t_hold, 3),
                                       "diag_rtt_ms": self.last_diag_ms, "metrics": rt_values})
                for k, v in d.items():
                    if not isinstance(v, int):
                        continue
                    # Free-space and largest-block diagnostics are safety floors; retain
                    # their minimum over the level. Other counters/latency maxima retain max.
                    is_floor = "free" in k or "largest" in k
                    if k not in peak or (v < peak[k] if is_floor else v > peak[k]):
                        peak[k] = v
                why = self.check_stops(d)
                if why:
                    self.stopped = why
                elif (self.a.max_rtt_ms is not None and self.lat and
                      max(self.lat) * 1000 > self.a.max_rtt_ms):
                    self.stopped = (f"X11 reply RTT {max(self.lat) * 1000:.1f} ms exceeded "
                                    f"--max-rtt-ms {self.a.max_rtt_ms:g}")
                elif self.a.max_diag_ms is not None and self.last_diag_ms is not None and self.last_diag_ms > self.a.max_diag_ms:
                    self.stopped = (f"diagnostics response {self.last_diag_ms:.1f} ms exceeded "
                                    f"--max-diag-ms {self.a.max_diag_ms:g}")
        # Stop generating work, then give already-sent replies/events a bounded chance to arrive.
        # Without this drain, a graphics tick at the 30 s boundary is counted as a lost event
        # simply because the client socket is closed immediately after the hold timer expires.
        workload_before_drain = self.workload
        self.workload = None
        drain_started = time.perf_counter()
        self.run_until(lambda: all(not c.pending and not c.event_pending for c in self.clients
                                   if c.state == "ready"), 0.5)
        event_drain_s = time.perf_counter() - drain_started
        pending_events_after_drain = sum(len(c.event_pending) for c in self.clients)
        self.workload = workload_before_drain
        hold = time.perf_counter() - t_hold
        self.measuring = False
        cpu2, rss2 = self.server_cpu_rss()
        loaded = samples[-1] if samples else {}
        survived = [c for c in ready if c.state == "ready"]
        lost = [c for c in ready if c.state != "ready"]
        clean_work = [c for c in survived if c.errors == 0 and
                      (workload != "graphics" or len(c.event_lat) == c.events_sent)]
        per_client_rtt = [len(c.rtt_lat) for c in survived]
        fairness_den = len(per_client_rtt) * sum(x * x for x in per_client_rtt)
        fairness = (sum(per_client_rtt) ** 2 / fairness_den) if fairness_den else None
        for c in self.clients:
            c.close()
        self.run_until(lambda: False, 1.5)  # the server notices the closes
        after = self.diag() or {}
        _, rss3 = self.server_cpu_rss()
        refused = {}
        for c in self.clients:
            if c.state == "failed" and c not in lost:
                refused[c.reason] = refused.get(c.reason, 0) + 1
        lost_reasons = {}
        for c in lost:
            lost_reasons[c.reason] = lost_reasons.get(c.reason, 0) + 1

        def delta(k, d=loaded):
            if not (isinstance(d.get(k), int) and isinstance(base.get(k), int)):
                return None
            v = d[k] - base[k]
            if k == "net.conn_bytes":  # without the monitor's own buffers
                v -= monitor_bytes(d, self.mon_slot) - monitor_bytes(base, self.mon_slot)
            return v

        mem_keys = ("net.conn_bytes", "net.core_used")
        per_conn = None
        if loaded and survived:
            used = sum(delta(k) or 0 for k in mem_keys)
            per_conn = used / len(survived)
        cleanup = None
        if after and base:
            cleanup = {k: delta(k, after) for k in
                       ("net.open", "net.conn_bytes", "net.core_used", "core.resources", "core.clients_open", "net.reserved", "net.queue_bytes")
                       if isinstance(after.get(k), int)}
        cpu_pct = (100.0 * (cpu2 - cpu1) / hold) if cpu1 is not None and cpu2 is not None and hold > 0 else None
        host_user0, host_user1 = base.get("host.cpu_user_ms"), loaded.get("host.cpu_user_ms")
        host_sys0, host_sys1 = base.get("host.cpu_sys_ms"), loaded.get("host.cpu_sys_ms")
        diag_ms = loaded.get("host.uptime_ms", 0) - base.get("host.uptime_ms", 0)
        if all(isinstance(x, int) for x in (host_user0, host_user1, host_sys0, host_sys1)) and diag_ms > 0:
            # The host server reports its own getrusage counters. This avoids ps TIME's
            # coarse resolution for short host runs.
            cpu_pct = 100.0 * ((host_user1 - host_user0) + (host_sys1 - host_sys0)) / diag_ms
        server_requests = delta("net.requests")
        counter_keys = ("net.graphics_requests", "net.graphics_us_total", "net.write_would_block",
                        "net.write_partial", "net.backpressure_waits", "net.passes_over_budget",
                        "net.passes_exceeded_request_budget")
        net_client_pass = {}
        for key, value in loaded.items():
            m = re.fullmatch(r"net\.conn\.(\d+)", key)
            if not m:
                continue
            fields = dict(re.findall(r"(pass_reqs(?:_max)?|service_us_max)=(\d+)", str(value)))
            if fields:
                net_client_pass[m.group(1)] = {k: int(v) for k, v in fields.items()}
        return {
            "workload": workload, "clients": n,
            "background_clients": self.a.background_clients,
            "x11_clients_total": n + 1 + self.a.background_clients,  # includes diag monitor
            "tcp_connected": sum(1 for c in self.clients if c.t_setup or c.state in ("ready", "closed")
                                 or getattr(c, "failed_in", "") == "setup"),
            "x11_set_up": len(ready), "setup_failures": n - len(ready), "refused": refused,
            "active_whole_hold": len(clean_work), "lost_during_hold": len(lost), "lost_reasons": lost_reasons,
            "burst_s": round(t_burst, 3),
            "setup_ms_p50": percentile(setup_ms, 50), "setup_ms_p95": percentile(setup_ms, 95),
            "setup_ms_max": max(setup_ms) if setup_ms else None,
            "hold_s": round(hold, 1), "requests_per_s": round(self.req_count / hold, 1) if hold else None,
            "per_client_rtt_counts": per_client_rtt,
            "per_client_rtt_count_min": min(per_client_rtt) if per_client_rtt else None,
            "per_client_rtt_count_max": max(per_client_rtt) if per_client_rtt else None,
            "per_client_rtt_jain_fairness": round(fairness, 4) if fairness is not None else None,
            "server_requests_per_s": round(server_requests / hold, 1) if server_requests is not None and hold else None,
            "rtt_ms_p50": round(1000 * percentile(self.lat, 50), 2) if self.lat else None,
            "rtt_ms_p95": round(1000 * percentile(self.lat, 95), 2) if self.lat else None,
            "rtt_ms_p99": round(1000 * percentile(self.lat, 99), 2) if self.lat else None,
            "diag_rtt_ms_p50": round(percentile(self.diag_lat, 50), 2) if self.diag_lat else None,
            "diag_rtt_ms_p95": round(percentile(self.diag_lat, 95), 2) if self.diag_lat else None,
            "diag_rtt_ms_p99": round(percentile(self.diag_lat, 99), 2) if self.diag_lat else None,
            "diag_rtt_ms_max": round(max(self.diag_lat), 2) if self.diag_lat else None,
            "x11_errors": self.error_codes,
            "reply_order_ok": all(c.reply_order_sent == c.reply_order_received for c in [self.monitor] + self.clients),
            "x11_events_received": sum(c.events for c in self.clients),
            "button_events_sent": sum(c.events_sent for c in self.clients),
            "button_events_received": sum(len(c.event_lat) for c in self.clients),
            "button_events_lost": sum(c.events_sent - len(c.event_lat) for c in self.clients),
            "event_drain_s": round(event_drain_s, 3),
            "pending_events_after_drain": pending_events_after_drain,
            "button_event_ms_p50": round(1000 * percentile([x for c in self.clients for x in c.event_lat], 50), 2)
            if any(c.event_lat for c in self.clients) else None,
            "button_event_ms_p95": round(1000 * percentile([x for c in self.clients for x in c.event_lat], 95), 2)
            if any(c.event_lat for c in self.clients) else None,
            "button_event_ms_p99": round(1000 * percentile([x for c in self.clients for x in c.event_lat], 99), 2)
            if any(c.event_lat for c in self.clients) else None,
            "button_event_ms_max": round(1000 * max((x for c in self.clients for x in c.event_lat), default=0), 2)
            if any(c.event_lat for c in self.clients) else None,
            "mem_per_conn_bytes": round(per_conn) if per_conn is not None else None,
            "conn_bytes": loaded.get("net.conn_bytes"), "core_used": loaded.get("net.core_used"),
            "reserved": loaded.get("net.reserved"), "reserve_per_conn": loaded.get("net.reserve_per_conn"),
            "total_peak": peak.get("net.total_peak"), "queue_peak_max": peak.get("net.queue_peak_max"),
            "server_cpu_pct": round(cpu_pct, 1) if cpu_pct is not None else None,
            "server_rss_bytes": rss2, "server_rss_after_bytes": rss3,
            "cleanup_delta": cleanup, "stopped": self.stopped,
            "extra_probe": extra_probe,
            "diag_peak": diag_peak_fields(peak),
            "rt_samples": rt_samples,
            "diag_delta": {k: (loaded[k] - base[k]) for k in counter_keys
                           if isinstance(loaded.get(k), int) and isinstance(base.get(k), int)},
            "net_client_pass": net_client_pass,
            "server_cpu_source": "host getrusage delta" if diag_ms > 0 else ("ps TIME delta" if cpu1 is not None else None),
        }

    def run(self):
        self.monitor = Client(self, -1)
        self.monitor.start()
        if not self.run_until(lambda: self.monitor.state in ("ready", "failed"), 10) or self.monitor.state != "ready":
            raise SystemExit(f"monitor connection failed: {self.monitor.reason}")
        self.run_until(lambda: bool(self.diag_atom), 5)
        if not self.diag_atom:
            raise SystemExit("the server has no _WOODLAND_DIAG")
        first = self.diag() or {}
        self.mon_slot = monitor_conn_slot(first, self.monitor.sock.getsockname()[1])
        if self.mon_slot is None:
            raise SystemExit("cannot identify diagnostics connection in _WOODLAND_DIAG; refusing ambiguous memory accounting")
        results = []
        workloads = ["idle", "protocol", "graphics"] if self.a.workload == "all" else [self.a.workload]
        for w in workloads:
            for n in self.a.levels:
                if self.stopped:
                    break
                r = self.level(w, n)
                results.append(r)
                print(row(r), flush=True)
                if r["extra_probe"]:
                    print(f"  extra X11 setup: {r['extra_probe']['state']} {r['extra_probe']['reason']}", flush=True)
                if self.a.json:
                    with open(self.a.json, "w") as f:
                        json.dump({"display": self.a.display, "rate": self.a.rate, "hold": self.a.hold,
                                   "results": results}, f, indent=1)
                if r["stopped"]:
                    print(f"STOPPED: threshold {r['stopped']}", flush=True)
        self.monitor.close()
        return results


HEADER = ("workload  clients total tcp  x11  whole lost  setup p50/max ms  req/s   rtt p50/p95/p99 ms   "
          "event p95/max ms  ev lost  mem/conn  core+conn bytes  cpu%   cleanup")


def row(r):
    f = lambda v, fmt="{}": "-" if v is None else fmt.format(v)
    cleanup = r["cleanup_delta"]
    clean = "-" if cleanup is None else ("ok" if all(v == 0 for v in cleanup.values()) else
                                         " ".join(f"{k}{v:+d}" for k, v in cleanup.items() if v))
    mem = None if r["conn_bytes"] is None else r["conn_bytes"] + (r["core_used"] or 0)
    return (f"{r['workload']:<9} {r['clients']:>7} {r['x11_clients_total']:>5} "
            f"{r['tcp_connected']:>4} {r['x11_set_up']:>4} "
            f"{r['active_whole_hold']:>5} {r['lost_during_hold']:>4}  "
            f"{f(r['setup_ms_p50'], '{:.1f}'):>6}/{f(r['setup_ms_max'], '{:.1f}'):<8}  {f(r['requests_per_s']):>6}  "
            f"{f(r['rtt_ms_p50']):>6}/{f(r['rtt_ms_p95']):>6}/{f(r['rtt_ms_p99']):>6}  "
            f"{f(r['button_event_ms_p95']):>6}/{f(r['button_event_ms_max']):<6} {r['button_events_lost']:>7}  "
            f"{f(r['mem_per_conn_bytes']):>8}  "
            f"{f(mem):>15}  {f(r['server_cpu_pct']):>5}  {clean}"
            + (f"  refused={r['refused']}" if r["refused"] else "") + (f"  lost={r['lost_reasons']}" if r["lost_reasons"] else "")
            + (f"  errors={r['x11_errors']}" if r["x11_errors"] else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--display", required=True)
    ap.add_argument("--no-auth", action="store_true", help="no cookie (host test server)")
    ap.add_argument("--xauthority", help="read the display cookie from this private Xauthority file")
    ap.add_argument("--probe-extra", action="store_true",
                    help="at the largest level, attempt one more genuine X11 setup while all clients remain connected")
    ap.add_argument("--background-clients", type=int, default=0,
                    help="already-connected X11 clients outside this benchmark (for total slot accounting)")
    ap.add_argument("--levels", default="4,8,16,32,64,128")
    ap.add_argument("--workload", choices=["idle", "protocol", "graphics", "fullscreen", "copy", "all"], default="all")
    ap.add_argument("--hold", type=float, default=60.0, help="seconds per level")
    ap.add_argument("--rate", type=float, default=10.0, help="workload operations per client per second")
    ap.add_argument("--diag-every", type=float, default=5.0)
    ap.add_argument("--diag-timeout", type=float, default=10.0,
                    help="maximum wait for one diagnostics reply (hardware runs should use a short bound)")
    ap.add_argument("--max-diag-ms", type=float,
                    help="stop the level after a diagnostics reply exceeds this latency")
    ap.add_argument("--max-rtt-ms", type=float,
                    help="stop the level after an X11 reply RTT exceeds this latency")
    ap.add_argument("--connect-timeout", type=float, default=20.0)
    ap.add_argument("--server-pid", type=int, help="host server pid, for CPU and RSS (ps)")
    ap.add_argument("--stop", action="append", default=[], help="KEY<VALUE or KEY>VALUE stop threshold")
    ap.add_argument("--json", help="write results here (after each level)")
    a = ap.parse_args()
    if a.background_clients < 0:
        ap.error("--background-clients must be nonnegative")
    if a.diag_timeout <= 0 or a.diag_every <= 0:
        ap.error("--diag-timeout and --diag-every must be positive")
    if a.max_diag_ms is not None and a.max_diag_ms <= 0:
        ap.error("--max-diag-ms must be positive")
    if a.max_rtt_ms is not None and a.max_rtt_ms <= 0:
        ap.error("--max-rtt-ms must be positive")
    a.levels = [int(x) for x in a.levels.split(",") if x]
    print(HEADER, flush=True)
    Bench(a).run()


if __name__ == "__main__":
    main()
