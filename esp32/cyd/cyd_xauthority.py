#!/usr/bin/env python3
"""Create a private Xauthority entry for CYD's standard TCP X11 display.

The cookie is read from a private hex file and sent to xauth over stdin, never in argv
or normal output. IPv4 literals are required to make FamilyInternet matching predictable.
"""
from __future__ import annotations

import argparse
import ipaddress
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def create_authority(host: str, display: int, cookie_file: Path, output: Path,
                     *, replace: bool = False, xauth: str | None = None) -> None:
    address = str(ipaddress.IPv4Address(host))
    if not 0 <= display <= 65535:
        raise ValueError("display must be between 0 and 65535")
    if output.exists() and not replace:
        raise FileExistsError(f"refusing to overwrite existing authority file: {output}")
    st = cookie_file.stat()
    if st.st_mode & 0o077:
        raise PermissionError("cookie file must be private (mode 0600 or stricter)")
    text = cookie_file.read_text(encoding="ascii").strip()
    if len(text) != 32:
        raise ValueError("cookie file must contain exactly 32 hexadecimal characters")
    try:
        cookie = bytes.fromhex(text)
    except ValueError as exc:
        raise ValueError("cookie file is not hexadecimal") from exc
    if len(cookie) != 16:
        raise ValueError("MIT-MAGIC-COOKIE-1 must be 16 bytes")
    xauth_bin = xauth or shutil.which("xauth")
    if not xauth_bin:
        raise FileNotFoundError("xauth executable not found")

    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent)
    os.close(fd)
    temp = Path(temp_name)
    os.chmod(temp, 0o600)
    old_umask = os.umask(0o077)
    try:
        command = f"add {address}:{display} MIT-MAGIC-COOKIE-1 {cookie.hex()}\nexit\n"
        proc = subprocess.run([xauth_bin, "-f", str(temp)], input=command, text=True,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=5, check=False)
        if proc.returncode:
            raise RuntimeError(f"xauth failed with exit status {proc.returncode}")
        os.chmod(temp, 0o600)
        os.replace(temp, output)
        os.chmod(output, 0o600)
    finally:
        os.umask(old_umask)
        try:
            temp.unlink()
        except FileNotFoundError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="numeric IPv4 address used in DISPLAY")
    parser.add_argument("--display", type=int, default=0)
    parser.add_argument("--cookie-file", type=Path, required=True, help="private 32-hex-digit file")
    parser.add_argument("--output", type=Path, required=True, help="new Xauthority database")
    parser.add_argument("--replace", action="store_true", help="replace this output file")
    args = parser.parse_args()
    try:
        create_authority(args.host, args.display, args.cookie_file, args.output, replace=args.replace)
    except (OSError, ValueError, RuntimeError) as exc:
        parser.error(str(exc))
    print(f"Xauthority entry created for {args.host}:{args.display}; mode 0600")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
