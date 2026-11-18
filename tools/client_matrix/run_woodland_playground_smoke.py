#!/usr/bin/env python3
"""Run the Xlib playground against Xvfb and the Woodland host server, with no WM."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIR = Path(__file__).resolve().parent
SERVER = ROOT / "build/x11_host_server"
Xvfb = shutil.which("Xvfb") or ("/opt/local/bin/Xvfb" if Path("/opt/local/bin/Xvfb").exists() else None)


class Output:
    def __init__(self, stream):
        self.lines: list[str] = []
        self.q: queue.Queue[str | None] = queue.Queue()

        def read_lines():
            for line in stream:
                line = line.rstrip("\n")
                self.lines.append(line)
                self.q.put(line)
            self.q.put(None)

        threading.Thread(target=read_lines, daemon=True).start()

    def wait_for(self, fragment: str, timeout: float = 5.0) -> str:
        deadline = time.monotonic() + timeout
        pending = list(self.lines)
        while time.monotonic() < deadline:
            for line in pending:
                if fragment in line:
                    return line
            pending.clear()
            try:
                line = self.q.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if line is None:
                break
            pending.append(line)
            if fragment in line:
                return line
        raise RuntimeError(f"did not see {fragment!r}; output so far: {self.lines}")


def free_display(tcp: bool = True) -> int:
    for number in range(190, 590):
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", 6000 + number))
            except OSError:
                continue
        if not tcp:
            return number
        return number
    raise RuntimeError("no free display number")


def wait_tcp(port: int, proc: subprocess.Popen, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"X server exited early: {proc.returncode}")
        with socket.socket() as sock:
            if sock.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.03)
    raise RuntimeError(f"X server did not listen on TCP port {port}")


def wait_x_socket(display: int, proc: subprocess.Popen, timeout: float = 5.0) -> None:
    path = Path(f"/tmp/.X11-unix/X{display}")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"Xvfb exited early: {proc.returncode}")
        if path.exists():
            return
        time.sleep(0.03)
    raise RuntimeError(f"Xvfb socket did not appear: {path}")


def inject_host(server: subprocess.Popen, x: int, y: int, buttons: int) -> None:
    assert server.stdin is not None
    server.stdin.write(f"pointer {x} {y} {buttons}\n")
    server.stdin.flush()
    time.sleep(0.10)


def inject_xvfb(env: dict[str, str], *args: str) -> None:
    subprocess.run(["xdotool", *args], env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=3)


def do_case(kind: str, app: Path, out: Path) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    display_num = free_display()
    display = f"127.0.0.1:{display_num}.0"
    image = out / f"woodland-playground-{kind}.png"
    server_log_path = out / f"woodland-playground-{kind}-server.log"
    server = None
    app_proc = None
    xvfb_log = None
    server_log = None
    try:
        if kind == "xvfb":
            xvfb_log = (out / "woodland-playground-xvfb.log").open("w")
            server = subprocess.Popen([Xvfb, f":{display_num}", "-screen", "0", "320x240x16",
                                       "-ac", "-listen", "tcp"], stdout=xvfb_log,
                                      stderr=subprocess.STDOUT)
            wait_x_socket(display_num, server)
            wait_tcp(6000 + display_num, server)
        else:
            server_log = server_log_path.open("w+")
            server = subprocess.Popen([str(SERVER), "-d", str(display_num), "--input", "--png",
                                       str(out / f"woodland-playground-{kind}-server.png"),
                                       "--clients", "4", "--resources", "256",
                                       "--client-resources", "128", "--budget-mb", "8", "-t"],
                                      stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                      stderr=server_log, text=True)
            wait_tcp(6000 + display_num, server)

        env = dict(os.environ, DISPLAY=display)
        app_proc = subprocess.Popen([str(app)], env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, bufsize=1)
        assert app_proc.stdout is not None
        output = Output(app_proc.stdout)
        output.wait_for("CONNECTED")
        output.wait_for("MAPPED")

        if kind == "xvfb":
            inject_xvfb(env, "mousemove", "54", "64", "click", "1")
        else:
            inject_host(server, 54, 64, 1)
            inject_host(server, 54, 64, 0)
        output.wait_for("CLICK count=1")
        if kind == "host":
            time.sleep(0.30)
            png_before_drag = (out / f"woodland-playground-{kind}-server.png").stat().st_mtime_ns

        if kind == "xvfb":
            inject_xvfb(env, "mousemove", "22", "130", "mousedown", "1")
            inject_xvfb(env, "mousemove", "65", "150")
            inject_xvfb(env, "mousemove", "108", "125")
            inject_xvfb(env, "mouseup", "1")
        else:
            inject_host(server, 22, 130, 1)
            inject_host(server, 65, 150, 1)
            inject_host(server, 108, 125, 1)
            inject_host(server, 108, 125, 0)
        drag_line = output.wait_for("DRAG points=", timeout=5)
        if kind == "xvfb":
            subprocess.run([sys.executable, str(ROOT / "tools/screenshot.py"), "--display", display,
                            "--no-auth", "--out", str(image)], check=True, timeout=8)
        else:
            server_image = out / f"woodland-playground-{kind}-server.png"
            # The client's DRAG line is emitted only after XSync, so its final
            # drawing requests have reached the server. Let the host server's
            # rate-limited PNG writer publish that completed frame.
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if server_image.exists() and server_image.stat().st_mtime_ns > png_before_drag:
                    time.sleep(0.35)
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("host server did not save a post-drag framebuffer image")
            shutil.copy2(server_image, image)

        if kind == "xvfb":
            inject_xvfb(env, "mousemove", "150", "64", "click", "1")
        else:
            inject_host(server, 150, 64, 1)
            inject_host(server, 150, 64, 0)
        output.wait_for("CLEAR points=0")

        if kind == "xvfb":
            inject_xvfb(env, "mousemove", "250", "64", "click", "1")
        else:
            inject_host(server, 250, 64, 1)
            inject_host(server, 250, 64, 0)
        output.wait_for("QUIT")
        app_status = app_proc.wait(timeout=5)
        stderr = app_proc.stderr.read() if app_proc.stderr else ""
        if app_status != 0 or stderr.strip():
            raise RuntimeError(f"client exit={app_status}, stderr={stderr!r}, output={output.lines}")

        sys.path.insert(0, str(ROOT / "tests"))
        from x11_raw import Raw
        inspect = Raw(6000 + display_num)
        try:
            _, children = inspect.query_tree(inspect.root)
            if inspect.errors:
                raise RuntimeError(f"QueryTree cleanup check got X errors: {inspect.errors}")
            if children:
                raise RuntimeError(f"test windows remained after exit: {children}")
        finally:
            inspect.close()

        result = {"case": kind, "display": display, "image": str(image),
                  "client_output": output.lines, "drag": drag_line,
                  "root_children_after_exit": 0, "client_exit": app_status,
                  "server_log": str(server_log_path) if kind == "host" else None}
        print(result)
        return result
    finally:
        if app_proc is not None and app_proc.poll() is None:
            app_proc.terminate()
            try:
                app_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                app_proc.kill()
                app_proc.wait()
        if server is not None:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        if server_log is not None:
            server_log.close()
        if xvfb_log is not None:
            xvfb_log.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    if not Xvfb:
        raise SystemExit("Xvfb not found on PATH or at /opt/local/bin/Xvfb")
    for command in ("xdotool",):
        if subprocess.run(["sh", "-c", f"command -v {command} >/dev/null 2>&1"]).returncode:
            raise SystemExit(f"required host test command missing: {command}")
    subprocess.run(["make", "-C", str(ROOT), "build/x11_host_server"], check=True)
    build = ROOT / "build/woodland_x11_playground"
    subprocess.run([str(SOURCE_DIR / "build_woodland_playground.sh"), str(build)], check=True)
    results = [do_case("xvfb", build, args.out_dir), do_case("host", build, args.out_dir)]
    for item in results:
        if item["server_log"]:
            log = Path(item["server_log"]).read_text(errors="replace")
            item["server_errors"] = [line for line in log.splitlines() if " -> Bad" in line or " error=" in line]
        else:
            item["server_errors"] = []
    summary = args.out_dir / "results.json"
    summary.write_text(json.dumps(results, indent=2) + "\n")
    print(f"results: {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
