#!/usr/bin/env python3
"""Build and exercise two small clients against an isolated Woodland host server."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "build/x11_host_server"
SOURCE = Path(__file__).resolve().parent


def display_number():
    for number in range(190, 590):
        with socket.socket() as sock:
            try:
                sock.bind(("127.0.0.1", 6000 + number))
            except OSError:
                continue
        return number
    raise RuntimeError("no free host display port")


def build(name):
    package = {"xlib": "x11", "xaw": "xaw7", "sdl2": "sdl2"}.get(name, name)
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", package], text=True).split()
    out = ROOT / "build" / (name + "_matrix_demo")
    subprocess.run(["cc", "-std=c99", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                    "-o", str(out), str(SOURCE / (name + "_demo.c")), *flags], check=True)
    return out


def probe(name, exe):
    number = display_number()
    with tempfile.TemporaryDirectory(prefix="woodland-client-matrix-") as directory:
        directory = Path(directory)
        with (directory / "server.log").open("w+") as log:
            server = subprocess.Popen([str(SERVER), "-d", str(number), "--input", "--png",
                                       str(directory / "screen.png"), "--clients", "6", "-t"],
                                      stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=log, text=True)
            try:
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    with socket.socket() as sock:
                        if sock.connect_ex(("127.0.0.1", 6000 + number)) == 0:
                            break
                    time.sleep(0.02)
                else:
                    raise RuntimeError("host server did not listen")
                env = dict(os.environ, DISPLAY=f"127.0.0.1:{number}.0", SDL_VIDEODRIVER="x11")
                client = subprocess.Popen([str(exe)], env=env, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, text=True)
                time.sleep(0.45)
                server.stdin.write("pointer 45 45 1\npointer 45 45 0\n")
                server.stdin.flush()
                stdout, stderr = client.communicate(timeout=6)
                time.sleep(0.25)
                log.flush()
                trace = (directory / "server.log").read_text()
                skipped = name == "sdl2" and "x11 not available" in stderr
                result = {"client": name, "exit": client.returncode, "skipped_host_backend": skipped,
                          "output": stdout.strip(),
                          "stderr": stderr.strip(), "server_errors": [line for line in trace.splitlines()
                                                            if " ! " in line or "error=" in line],
                          "png_written": (directory / "screen.png").exists()}
                print(json.dumps(result, indent=2))
                return skipped or (client.returncode == 0 and not result["server_errors"] and result["png_written"])
            finally:
                server.terminate()
                try:
                    server.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.communicate()


if __name__ == "__main__":
    if not SERVER.exists():
        raise SystemExit("build the host server first: make -C experiments/x11-esp32 build/x11_host_server")
    results = [probe(name, build(name)) for name in ("xlib", "xcb", "xaw", "sdl2")]
    raise SystemExit(0 if all(results) else 1)
