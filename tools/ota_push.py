#!/usr/bin/env python3
"""Woodland X11 firmware update over the LAN, protocol WLOTA2 (block-acknowledged; see ota/x11_ota.h).

    tools/ota_push.py --host PANEL_IP --key-file KEY status
    tools/ota_push.py --host PANEL_IP --key-file KEY upload build/esp32-ab/x11_esp32.ino.bin [--reboot] [--block 16384]
    tools/ota_push.py --host PANEL_IP --key-file KEY reboot
    tools/ota_push.py --host PANEL_IP --key-file KEY boot-other      (switch back to the other slot)
    tools/ota_push.py check IMAGE                                         (local checks only)

KEY: a file holding the device's 32-byte OTA key as 64 hex digits, mode 0600. It is never
printed or sent: requests carry HMAC-SHA256(key, device nonce || request); every message from
the device (replies and block ACKs) is checked the same way, including its index in the session,
and each ACK must name the block just sent and the exact offset now committed.

Uploads go block by block: one block (4 KiB or 16 KiB) is sent, then the tool waits until the
device has written it to flash and acknowledged it, so no data is in flight while the device's
flash is busy. The image is checked locally first (ESP32-S3 app, IDF version, OTA receiver
marker, SHA-256 in the authenticated request); the device checks everything again, writes only
its inactive slot, and selects it only after the whole image validated.

Protocol 1 (WLOTA1) devices are refused: update them over USB first."""
from __future__ import annotations

import argparse
import hashlib
import hmac
import os
import secrets
import socket
import stat
import struct
import sys
import time

PORT = 5232
MARKER = b"WLX11-OTA-RECEIVER-1"
CMDS = {"upload": 1, "status": 2, "boot-other": 3, "reboot": 4}
STATUS = ["ok", "authentication failed", "locked out", "no inactive slot", "image too large", "incompatible image",
          "bad request", "busy", "flash error", "hash mismatch", "invalid image",
          "image cannot receive OTA (marker missing)", "timeout", "not possible"]
BLOCK_SIZES = (4096, 16384)
ACK_TIMEOUT = 60.0   # one block: receive + erase/program of up to 4 sectors, with margin
# WLOTA2 devices number their signed messages within a session in one byte (index 0..255, inside
# the MAC). An index must never repeat in a session, so a session may carry at most 256 of them.
MAX_SIGNED_MESSAGES = 256


class OtaError(Exception):
    pass


def signed_messages(size: int, block: int) -> int:
    """Signed messages a WLOTA2 device sends in one upload session: the reply to the request
    ("send blocks"), one ACK per block, and the final verdict (an error reply instead of an
    ACK or verdict ends the session early, so it never adds one). The unsigned HELLO is not
    numbered."""
    return 1 + -(-size // block) + 1


def check_message_budget(size: int, block: int) -> int:
    n = signed_messages(size, block)
    if n > MAX_SIGNED_MESSAGES:
        fits = [b for b in BLOCK_SIZES if signed_messages(size, b) <= MAX_SIGNED_MESSAGES]
        raise OtaError(f"{size} bytes in {block}-byte blocks needs {n} signed device messages; protocol 2 numbers "
                       f"at most {MAX_SIGNED_MESSAGES} per session" + (f": use --block {max(fits)}" if fits else ""))
    return n


def read_key(path: str) -> bytes:
    st = os.stat(path)
    if st.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise OtaError(f"{path}: must be private (chmod 600)")
    with open(path) as f:
        text = f.read().strip()
    if len(text) != 64:
        raise OtaError(f"{path}: expected 64 hex digits")
    return bytes.fromhex(text)


def image_info(img: bytes) -> dict:
    if len(img) < 512 or img[0] != 0xE9:
        raise OtaError("not an ESP application image")
    if struct.unpack_from("<H", img, 12)[0] != 9:
        raise OtaError("not an ESP32-S3 image")
    if struct.unpack_from("<I", img, 32)[0] != 0xABCD5432:
        raise OtaError("no application descriptor")
    idf = img[32 + 112:32 + 144].split(b"\0")[0]
    if MARKER not in img:
        raise OtaError("this firmware has no OTA receiver: after it, only USB could update the panel")
    return {"size": len(img), "sha256": hashlib.sha256(img).digest(), "idf": idf,
            "build_id": img[32 + 144:32 + 176]}


def recv_exact(s: socket.socket, n: int) -> bytes:
    b = b""
    while len(b) < n:
        part = s.recv(n - len(b))
        if not part:
            raise OtaError(f"connection closed by the device after {len(b)} of {n} bytes")
        b += part
    return b


class Session:
    def __init__(self, host: str, port: int, key: bytes, timeout: float = 30.0):
        self.key = key
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        head = recv_exact(self.s, 8)
        if head in (b"WLOTA2RS", b"WLOTA1RS"):  # busy
            rest = recv_exact(self.s, 88)
            raise OtaError("device busy: " + STATUS[min(struct.unpack_from("<I", head + rest, 8)[0], len(STATUS) - 1)])
        if head == b"WLOTA1\r\n":
            raise OtaError("the device speaks protocol 1 (old firmware): install a WLOTA2 build over USB first")
        if head != b"WLOTA2\r\n":
            raise OtaError("not a WLOTA2 endpoint")
        hello = head + recv_exact(self.s, 56)
        self.version, self.flags, self.slot_size = struct.unpack_from("<HHI", hello, 8)
        self.server_nonce = hello[16:32]
        self.running = hello[32]
        self.block_default, self.block_max = struct.unpack_from("<II", hello, 36)
        self.client_nonce = secrets.token_bytes(16)
        self.replies = 0  # index the next message from the device must carry

    def request(self, cmd: int, info: dict | None = None, block: int = 16384, mac_key: bytes | None = None) -> None:
        info = info or {"size": 0, "sha256": bytes(32), "idf": b"", "build_id": bytes(32)}
        body = (b"WLOTA2RQ" + bytes([cmd, 0, 0, 0]) + self.client_nonce + struct.pack("<II", info["size"], block)
                + info["sha256"] + MARKER.ljust(32, b"\0") + info["idf"][:31].ljust(32, b"\0")
                + info["build_id"][:28].ljust(28, b"\0"))
        assert len(body) == 160
        mac = hmac.new(mac_key if mac_key is not None else self.key, self.server_nonce + body, hashlib.sha256).digest()
        self.s.sendall(body + mac)

    def message(self) -> tuple[bytes, int, bytes]:
        """Reads and authenticates one message (REPLY or ACK): returns (magic, status, raw)."""
        self.last_raw = recv_exact(self.s, 96)
        return self.check(self.last_raw)

    def check(self, r: bytes) -> tuple[bytes, int, bytes]:
        magic = r[:8]
        if magic not in (b"WLOTA2RS", b"WLOTA2AK"):
            raise OtaError("malformed message from the device")
        status = struct.unpack_from("<I", r, 8)[0]
        mac = r[64:96]
        if self.replies >= MAX_SIGNED_MESSAGES:  # index 256 would arrive as 0: never accept a repeated index
            raise OtaError(f"more than {MAX_SIGNED_MESSAGES} device messages in one session: the 8-bit position would repeat")
        if mac != bytes(32):
            want = hmac.new(self.key, self.client_nonce + self.server_nonce + r[:64], hashlib.sha256).digest()
            if not hmac.compare_digest(mac, want):
                raise OtaError("the device's message does not authenticate: wrong device or key")
            if r[63] != self.replies:
                raise OtaError(f"message out of order (index {r[63]}, expected {self.replies}): replayed?")
        elif status == 0 or magic == b"WLOTA2AK":
            raise OtaError("unauthenticated OK/ACK: refusing to trust it")
        self.replies += 1
        return magic, status, r

    def reply(self) -> tuple[int, int, str]:
        magic, status, r = self.message()
        if magic != b"WLOTA2RS":
            raise OtaError("expected a reply, got an ACK")
        return status, struct.unpack_from("<I", r, 12)[0], r[16:63].split(b"\0")[0].decode(errors="replace")

    def close(self):
        self.s.close()


def upload_blocks(s: Session, img: bytes, block: int, progress: bool, timings: dict) -> tuple[int, str]:
    """Sends the image block by block; each block only after the previous one's ACK."""
    check_message_budget(len(img), block)
    s.s.settimeout(ACK_TIMEOUT)
    off, seq = 0, 0
    t0 = time.monotonic()
    while off < len(img):
        n = min(block, len(img) - off)
        t = time.monotonic()
        s.s.sendall(b"WLB2" + struct.pack("<III", seq, off, n) + img[off:off + n])
        magic, status, r = s.message()
        if magic == b"WLOTA2RS":  # the device ended the session (an error)
            return status, r[16:63].split(b"\0")[0].decode(errors="replace")
        a_seq, a_off, recv, wait, erase, prog = struct.unpack_from("<IIIIII", r, 12)
        if status != 0 or a_seq != seq or a_off != off + n:
            raise OtaError(f"bad ACK for block {seq}: status {status}, block {a_seq}, offset {a_off} (expected {off + n})")
        rtt = time.monotonic() - t
        for k, v in (("recv_us", recv), ("wait_us", wait), ("erase_us", erase), ("program_us", prog)):
            timings[k] = timings.get(k, 0) + v
        timings["rtt_max_ms"] = max(timings.get("rtt_max_ms", 0), rtt * 1000)
        off += n
        seq += 1
        if progress:
            rate = off / max(time.monotonic() - t0, 1e-6)
            print(f"\r{off} / {len(img)} bytes, {seq} blocks, {rate / 1024:.1f} KiB/s", end="", file=sys.stderr, flush=True)
    if progress:
        print(file=sys.stderr)
    timings["seconds"] = time.monotonic() - t0
    timings["blocks"] = seq
    st, detail, text = s.reply()  # the final verdict: hash, validation, boot slot
    return st, text


def run(host, port, key, cmd, image=None, reboot=False, progress=True, block=16384, timings=None):
    timings = {} if timings is None else timings
    if cmd == "upload":
        if block not in BLOCK_SIZES:
            raise OtaError(f"block size must be one of {BLOCK_SIZES}")
        with open(image, "rb") as f:
            img = f.read()
        info = image_info(img)
        check_message_budget(info["size"], block)  # before connecting: nothing starts that cannot finish
    s = Session(host, port, key)
    try:
        if cmd != "upload":
            s.request(CMDS[cmd])
            st, detail, text = s.reply()
            return st, text
        if not s.flags & 1:
            raise OtaError("the device has no inactive slot (single-slot partition table: migrate over USB first)")
        if info["size"] > s.slot_size:
            raise OtaError(f"image {info['size']} bytes, slot {s.slot_size}")
        if block > s.block_max:
            raise OtaError(f"the device accepts blocks up to {s.block_max} bytes")
        s.request(1, info, block)
        st, detail, text = s.reply()
        if st != 0:
            return st, text
        st, text = upload_blocks(s, img, block, progress, timings)
    finally:
        s.close()
    if st == 0 and reboot:
        return run(host, port, key, "reboot")
    return st, text


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--key-file")
    ap.add_argument("--reboot", action="store_true", help="upload: reboot into the new image afterwards")
    ap.add_argument("--block", type=int, default=16384, help="upload block size: 4096 or 16384")
    ap.add_argument("command", choices=["upload", "status", "reboot", "boot-other", "check"])
    ap.add_argument("image", nargs="?")
    a = ap.parse_args()
    timings: dict = {}
    try:
        if a.command == "check":
            with open(a.image, "rb") as f:
                info = image_info(f.read())
            print(f"ok: {info['size']} bytes, sha256 {info['sha256'].hex()}, IDF {info['idf'].decode()}, "
                  f"build {info['build_id'].hex()[:16]}, OTA receiver marker present")
            for b in BLOCK_SIZES:
                n = signed_messages(info["size"], b)
                print(f"  --block {b}: {n - 2} blocks, {n} signed device messages (limit {MAX_SIGNED_MESSAGES}): "
                      + ("ok" if n <= MAX_SIGNED_MESSAGES else "refused"))
            return 0
        if not a.key_file:
            raise OtaError("--key-file is required")
        if a.command == "upload" and not a.image:
            raise OtaError("upload needs an image")
        st, text = run(a.host, a.port, read_key(a.key_file), a.command, a.image, a.reboot, block=a.block, timings=timings)
    except (OtaError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if timings.get("blocks"):
        print(f"{timings['blocks']} blocks in {timings['seconds']:.1f} s; device: receive {timings['recv_us'] / 1e3:.0f} ms, "
              f"waiting for the next block {timings['wait_us'] / 1e3:.0f} ms, erase {timings['erase_us'] / 1e3:.0f} ms, "
              f"program {timings['program_us'] / 1e3:.0f} ms; slowest block round trip {timings['rtt_max_ms']:.0f} ms")
    print(f"{STATUS[st] if st < len(STATUS) else st}: {text}")
    return 0 if st == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
