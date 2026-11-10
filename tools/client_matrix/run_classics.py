#!/usr/bin/env python3
"""Short, isolated startup/paint smoke for installed classic X clients.

This is deliberately weaker than run.py: it does not validate pixels or input.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from run import SERVER, display_number

CASES = {
    "xclock": ["xclock", "-analog", "-update", "1", "-geometry", "160x160+30+30"],
    "xeyes": ["xeyes", "-geometry", "160x100+30+30"],
    "xcalc": ["xcalc", "-geometry", "+30+30"],
    "xload": ["xload", "-geometry", "160x100+30+30"],
    "xterm": ["xterm", "-fn", "6x12", "-geometry", "28x6+30+30", "-e", "sleep", "5"],
    "blackbox": ["blackbox"],
}


def run(name, argv):
    if not shutil.which(argv[0]):
        return {"client": name, "result": "not installed"}
    number = display_number()
    with tempfile.TemporaryDirectory(prefix="woodland-classic-") as tmp:
        tmp = Path(tmp)
        with (tmp / "server.log").open("w+") as log:
            server = subprocess.Popen([str(SERVER), "-d", str(number), "-t", "--png", str(tmp / "screen.png"),
                                       "--clients", "6"], stdout=subprocess.DEVNULL, stderr=log)
            try:
                time.sleep(0.25)
                env = dict(os.environ, DISPLAY=f"127.0.0.1:{number}.0", LC_ALL="C")
                client = subprocess.Popen(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                time.sleep(2.2)
                alive = client.poll() is None
                if alive:
                    client.terminate()
                out, err = client.communicate(timeout=3)
                time.sleep(0.2)
                log.flush()
                lines = (tmp / "server.log").read_text().splitlines()
                return {"client": name, "alive_at_2s": alive,
                        "create_window": sum("CreateWindow" in line for line in lines),
                        "map_window": sum("MapWindow" in line for line in lines),
                        "draw_requests": sum(any(op in line for op in ("PolyFillRectangle", "PolyLine", "PolyArc",
                                                                        "FillPoly", "ImageText8", "PutImage")) for line in lines),
                        "error_lines": [line for line in lines if " ! " in line or "error=" in line][:25],
                        "png_written": (tmp / "screen.png").exists(), "client_stderr": err[:500],
                        "client_stdout": out[:100]}
            finally:
                server.terminate()
                server.communicate(timeout=3)


if __name__ == "__main__":
    if not SERVER.exists():
        raise SystemExit("build the host server first")
    for name, argv in CASES.items():
        print(json.dumps(run(name, argv)))
