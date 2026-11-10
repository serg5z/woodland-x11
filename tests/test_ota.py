"""OTA update tests against the simulated CrowPanel (build/ota_device_sim, ASan/UBSan) with the
real client tool (tools/ota_push.py).

SIMULATION: the device side is the real protocol engine (ota/x11_ota.c, the code the ESP32
runs) over a 16 MB flash file, with a model of the ESP-IDF v5.5 bootloader's OTA selection
and rollback (see host/ota_device_sim.c). These tests show the protocol, the checks, the
otadata bookkeeping and the rollback logic behave as designed; they do not prove the real
bootloader does the same - that is the hardware checklist's job.

    python3 -m unittest -v tests/test_ota.py   (make first)"""
from __future__ import annotations

import hashlib
import hmac
import os
import queue
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "tools"))
import ota_push  # noqa: E402

SIM = os.path.join(HERE, "build", "ota_device_sim")
GEN = os.path.expanduser("~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12/tools/gen_esp32part.py")
FLASH = 16 * 1024 * 1024
NVS = (0x9000, 0x5000)
OTADATA = 0xE000
AB = [("nvs", 1, 0x02, 0x9000, 0x5000), ("otadata", 1, 0x00, 0xE000, 0x2000), ("app0", 0, 0x10, 0x10000, 0x300000),
      ("app1", 0, 0x11, 0x310000, 0x300000), ("coredump", 1, 0x03, 0x610000, 0x10000)]
HUGE_APP = [("nvs", 1, 0x02, 0x9000, 0x5000), ("otadata", 1, 0x00, 0xE000, 0x2000),
            ("app0", 0, 0x10, 0x10000, 0x300000), ("spiffs", 1, 0x82, 0x310000, 0xE0000),
            ("coredump", 1, 0x03, 0x3F0000, 0x10000)]


def partition_table(entries) -> bytes:
    """ESP-IDF binary partition table (as gen_esp32part.py writes it), 0xC00 bytes."""
    t = b""
    for name, typ, sub, off, size in entries:
        t += b"\xaa\x50" + bytes([typ, sub]) + struct.pack("<II", off, size) + name.encode().ljust(16, b"\0") + bytes(4)
    t += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(t).digest()
    return t.ljust(0xC00, b"\xff")


def otadata_entry(seq: int, state: int = 0xFFFFFFFF) -> bytes:
    crc = zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF)
    return struct.pack("<I", seq) + b"\xff" * 20 + struct.pack("<II", state, crc)


def boot_app0() -> bytes:
    """Arduino's boot_app0.bin: copy 0 seq 1 (state undefined), copy 1 seq 0."""
    return (otadata_entry(1) + b"\xff" * (0x1000 - 32) + otadata_entry(0) + b"\xff" * (0x1000 - 32))


def make_image(tag: bytes, size: int = 300_000, idf: bytes = b"v5.5.5", marker: bool = True, chip: int = 9) -> bytes:
    """A valid ESP32-S3 application image: header, a segment with an app descriptor, a second
    segment, checksum, appended SHA-256 (the layout esptool elf2image writes)."""
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0, 0xABCD5432)
    desc[16:16 + len(tag)] = tag
    desc[112:112 + len(idf)] = idf
    desc[144:176] = hashlib.sha256(tag).digest()
    body = bytearray(os.urandom(size))
    if marker:
        body[size // 3:size // 3 + len(ota_push.MARKER)] = ota_push.MARKER
    segs = [bytes(desc) + bytes(body[: size // 2]), bytes(body[size // 2:])]
    hdr = bytearray(24)
    hdr[0], hdr[1], hdr[23] = 0xE9, len(segs), 1
    struct.pack_into("<H", hdr, 12, chip)
    img = bytes(hdr)
    ck = 0xEF
    for i, s in enumerate(segs):
        img += struct.pack("<II", 0x3C000020 + i * 0x100000, len(s)) + s
        for b in s:
            ck ^= b
    pad = (16 - (len(img) + 1) % 16) % 16
    img += bytes(pad) + bytes([ck])
    return img + hashlib.sha256(img).digest()


def make_flash(table, app0: bytes, nvs: bytes) -> bytes:
    f = bytearray(b"\xff" * FLASH)
    f[0x1000:0x1000 + 64] = b"BOOTLOADER-PLACEHOLDER (the simulator does not run it)".ljust(64, b"\0")
    f[0x8000:0x8C00] = partition_table(table)
    f[NVS[0]:NVS[0] + NVS[1]] = nvs
    f[OTADATA:OTADATA + 0x2000] = boot_app0()
    f[0x10000:0x10000 + len(app0)] = app0
    return bytes(f)


class Device:
    def __init__(self, tmp: str, flash: bytes, key: bytes, extra=()):
        self.path = os.path.join(tmp, "flash.bin")
        with open(self.path, "wb") as f:
            f.write(flash)
        self.key_path = os.path.join(tmp, "device.key")
        with open(os.open(self.key_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
            f.write(key.hex())
        self.port = 15232 + os.getpid() % 2000 + int(time.time() * 1000) % 3000
        self.p = subprocess.Popen([SIM, "--flash", self.path, "--key-file", self.key_path, "--port", str(self.port), *extra],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.lines: queue.Queue = queue.Queue()
        threading.Thread(target=lambda: [self.lines.put(l) for l in iter(self.p.stdout.readline, "")], daemon=True).start()
        self.err: list[str] = []
        threading.Thread(target=lambda: self.err.extend(iter(self.p.stderr.readline, "")), daemon=True).start()
        self.state = self.next_state()  # after the first boot

    def next_state(self, timeout: float = 10) -> dict:
        line = self.lines.get(timeout=timeout)
        d = {}
        for part in line.split()[1:]:
            k, _, v = part.partition("=")
            d[k] = v
        return d

    def stats(self) -> dict:
        self.p.stdin.write("stats\n")
        self.p.stdin.flush()
        line = self.lines.get(timeout=10)
        self.state = self.next_state()
        return {k: int(v) for k, _, v in (p.partition("=") for p in line.split()[1:])}

    def cmd(self, line: str) -> dict:
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        self.state = self.next_state()
        return self.state

    def flash(self) -> bytes:
        with open(self.path, "rb") as f:
            return f.read()

    def stop(self) -> str:
        if self.p.poll() is None:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
            self.p.wait(timeout=10)
        for stream in (self.p.stdin, self.p.stdout, self.p.stderr):
            stream.close()
        time.sleep(0.05)
        return "".join(self.err)


@unittest.skipUnless(os.path.exists(SIM), "make build/ota_device_sim first")
class OtaTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="ota")
        self.key = os.urandom(32)
        self.nvs = os.urandom(NVS[1])  # stands for Wi-Fi credentials, the X11 cookie, calibration
        self.a = make_image(b"build-A")
        self.dev = None

    def tearDown(self):
        if self.dev:
            err = self.dev.stop()
            self.assertNotIn("AddressSanitizer", err)
            self.assertNotIn("runtime error", err)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def start(self, table=AB, app0=None, extra=()):
        self.dev = Device(self.tmp, make_flash(table, app0 or self.a, self.nvs), self.key, extra)
        return self.dev

    def push(self, cmd, image=None, key=None, reboot=False, block=16384, timings=None):
        path = None
        if image is not None:
            path = os.path.join(self.tmp, "image.bin")
            with open(path, "wb") as f:
                f.write(image)
        st, text = ota_push.run("127.0.0.1", self.dev.port, key or self.key, cmd, path, reboot=False, progress=False,
                                block=block, timings=timings)
        if reboot and st == 0:
            st2, _ = ota_push.run("127.0.0.1", self.dev.port, key or self.key, "reboot", progress=False)
            self.assertEqual(st2, 0)
            self.dev.state = self.dev.next_state()  # the simulator reports after its reboot
        return st, text

    def nvs_intact(self):
        f = self.dev.flash()
        self.assertEqual(f[NVS[0]:NVS[0] + NVS[1]], self.nvs, "NVS (credentials, cookie) changed")

    def raw(self, cmd: int, size: int, sha: bytes, marker: bytes = ota_push.MARKER, idf: bytes = b"v5.5.5",
            key: bytes | None = None, block: int = 16384):
        """A WLOTA2 request built by hand (to bypass the tool's own checks)."""
        s = ota_push.Session("127.0.0.1", self.dev.port, self.key)
        body = (b"WLOTA2RQ" + bytes([cmd, 0, 0, 0]) + s.client_nonce + struct.pack("<II", size, block) + sha
                + marker.ljust(32, b"\0") + idf.ljust(32, b"\0") + bytes(28))
        s.s.sendall(body + hmac.new(key or self.key, s.server_nonce + body, hashlib.sha256).digest())
        return s

    # ---- the layout ----

    def test_table_matches_gen_esp32part(self):
        if not os.path.exists(GEN):
            self.skipTest("Arduino-ESP32 3.3.12 tools not installed")
        out = os.path.join(self.tmp, "pt.bin")
        subprocess.run([sys.executable, GEN, "--flash-size", "16MB", os.path.join(HERE, "esp32", "partitions_ab.csv"), out],
                       check=True, capture_output=True)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), partition_table(AB))

    def test_single_slot_layout_cannot_update(self):
        """The layout the panel runs now (huge_app): one app slot. The receiver says so; nothing is written."""
        dev = self.start(HUGE_APP)
        self.assertEqual(dev.state["running"], "0")
        before = dev.flash()
        with self.assertRaisesRegex(ota_push.OtaError, "no inactive slot"):
            self.push("upload", make_image(b"build-B"))
        st, text = self.push("status")
        self.assertEqual(st, 0)
        self.assertEqual(dev.flash(), before)

    # ---- updates ----

    def test_update_a_b_a_keeps_credentials(self):
        dev = self.start()
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("0", "undefined"))  # boot_app0: seq 1
        b = make_image(b"build-B")
        st, text = self.push("upload", b, reboot=True)
        self.assertEqual(st, 0, text)
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("1", "pending"))  # on trial
        self.assertEqual(dev.cmd("confirm")["entry"], "valid")
        self.assertEqual(dev.flash()[0x310000:0x310000 + len(b)], b)
        self.nvs_intact()
        a2 = make_image(b"build-A2")
        st, text = self.push("upload", a2, reboot=True)
        self.assertEqual(st, 0, text)
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("0", "pending"))
        self.assertEqual(dev.cmd("confirm")["entry"], "valid")
        self.assertEqual(dev.flash()[0x10000:0x10000 + len(a2)], a2)
        self.assertEqual(dev.flash()[0x310000:0x310000 + len(b)], b)  # the previous image stays for a rollback
        self.nvs_intact()

    def test_repeated_cycles(self):
        dev = self.start()
        for i in range(12):
            img = make_image(b"cycle-%d" % i, size=200_000 + 1000 * i)
            st, text = self.push("upload", img, reboot=True)
            self.assertEqual(st, 0, f"cycle {i}: {text}")
            self.assertEqual(dev.state["running"], str((i + 1) % 2))
            self.assertEqual(dev.cmd("confirm")["entry"], "valid")
            self.assertEqual(dev.cmd("boot")["running"], str((i + 1) % 2))  # a plain reset keeps it
        self.nvs_intact()

    def test_interrupted_transfer(self):
        dev = self.start()
        b = make_image(b"build-B")
        info = ota_push.image_info(b)
        s = self.raw(1, info["size"], info["sha256"], idf=info["idf"])
        self.assertEqual(s.reply()[0], 0)
        s.s.sendall(b"WLB2" + struct.pack("<III", 0, 0, 16384) + b[:16384])
        s.message()  # block 0 committed
        s.s.sendall(b"WLB2" + struct.pack("<III", 1, 16384, 16384) + b[16384:20000])
        s.s.close()  # the link drops in the middle of block 1
        time.sleep(0.3)
        self.assertEqual(dev.cmd("boot")["running"], "0")  # nothing selected: the old app runs on
        self.assertEqual(dev.state["ota1"].split(":")[0], "valid")  # copy 1 untouched (boot_app0's seq 0)
        st, text = self.push("upload", b, reboot=True)  # simply start again
        self.assertEqual(st, 0, text)
        self.assertEqual(dev.state["running"], "1")
        self.nvs_intact()

    def test_wrong_checksum(self):
        dev = self.start()
        b = make_image(b"build-B")
        info = ota_push.image_info(b)
        s = self.raw(1, info["size"], hashlib.sha256(b"something else").digest(), idf=info["idf"])
        self.assertEqual(s.reply()[0], 0)
        st, _ = ota_push.upload_blocks(s, b, 16384, False, {})
        self.assertEqual(st, ota_push.STATUS.index("hash mismatch"))
        s.close()
        self.assertEqual(dev.cmd("boot")["running"], "0")
        self.nvs_intact()

    def test_oversized_and_malformed(self):
        dev = self.start()
        cases = []
        s = self.raw(1, 0x300001, bytes(32))  # larger than the slot
        cases.append(("too large", s.reply()[0], "image too large"))
        s.close()
        bad = bytearray(make_image(b"bad-magic"))
        bad[0] = 0xE8
        cases.append(("bad magic", self.send_raw(bytes(bad)), "invalid image"))
        flipped = bytearray(make_image(b"flipped"))
        flipped[5000] ^= 1  # data changed after the image was built: checksum and appended hash fail
        cases.append(("corrupt data", self.send_raw(bytes(flipped)), "invalid image"))
        cases.append(("no marker", self.send_raw(make_image(b"no-marker", marker=False)),
                      "image cannot receive OTA (marker missing)"))
        cases.append(("other IDF", self.send_raw(make_image(b"idf", idf=b"v5.4.1"), idf=b"v5.4.1"),
                      "incompatible image"))
        cases.append(("other chip", self.send_raw(make_image(b"chip", chip=2)), "incompatible image"))
        cases.append(("truncated", self.send_raw(make_image(b"cut")[:-100]), "invalid image"))
        for name, got, want in cases:
            self.assertEqual(ota_push.STATUS[got], want, name)
        self.assertEqual(dev.cmd("boot")["running"], "0")
        self.assertEqual(dev.state["entry"], "undefined")  # otadata never touched
        self.nvs_intact()
        with self.assertRaisesRegex(ota_push.OtaError, "no OTA receiver"):  # the tool refuses it first
            self.push("upload", make_image(b"no-marker", marker=False))

    def send_raw(self, img: bytes, idf: bytes = b"v5.5.5") -> int:
        s = self.raw(1, len(img), hashlib.sha256(img).digest(), idf=idf)
        st = s.reply()[0]
        if st == 0:
            try:
                st, _ = ota_push.upload_blocks(s, img, 16384, False, {})
            except (ota_push.OtaError, OSError):
                pass
        s.close()
        return st

    def test_authentication(self):
        dev = self.start()
        wrong = os.urandom(32)
        before = dev.flash()
        st, text = self.push("upload", make_image(b"evil"), key=wrong)
        self.assertEqual(ota_push.STATUS[st], "authentication failed")
        self.assertEqual(dev.flash(), before)  # nothing written
        for _ in range(4):  # 5 failures within a minute
            self.push("status", key=wrong)
        st, _ = self.push("status")  # even the right key is locked out now
        self.assertEqual(ota_push.STATUS[st], "locked out")
        self.assertEqual(dev.flash(), before)

    def test_device_must_authenticate_too(self):
        """A reply whose MAC does not match (another device, or an attacker answering) is rejected by the tool."""
        self.start()
        s = ota_push.Session("127.0.0.1", self.dev.port, self.key)
        s.key = os.urandom(32)  # the tool now expects another key: the genuine device's MAC fails
        body = (b"WLOTA2RQ" + bytes([2, 0, 0, 0]) + s.client_nonce + bytes(4 + 4 + 32 + 32 + 32 + 28))
        s.s.sendall(body + hmac.new(self.key, s.server_nonce + body, hashlib.sha256).digest())
        with self.assertRaisesRegex(ota_push.OtaError, "does not authenticate"):
            s.reply()
        s.close()

    def test_replayed_reply_is_rejected(self):
        """An attacker on the path cannot replay the upload's first OK ("send the image") in
        place of its final reply: the reply index is inside the MAC."""
        self.start()
        b = make_image(b"build-B")
        info = ota_push.image_info(b)
        s = self.raw(1, info["size"], hashlib.sha256(b"not this image").digest(), idf=info["idf"])
        self.assertEqual(s.reply()[0], 0)
        first = s.last_raw
        st, _ = ota_push.upload_blocks(s, b, 16384, False, {})
        self.assertEqual(ota_push.STATUS[st], "hash mismatch")  # the genuine final reply
        s.replies = 1 + -(-len(b) // 16384)  # after the OK and one ACK per block
        with self.assertRaisesRegex(ota_push.OtaError, "out of order"):
            s.check(first)  # the replayed first reply, presented as the final one
        forged = bytearray(first)
        forged[63] = 1  # changing the index breaks the MAC
        with self.assertRaisesRegex(ota_push.OtaError, "does not authenticate"):
            s.check(bytes(forged))
        s.close()

    def test_busy(self):
        self.start()
        first = ota_push.Session("127.0.0.1", self.dev.port, self.key)
        with self.assertRaisesRegex(ota_push.OtaError, "busy"):
            ota_push.Session("127.0.0.1", self.dev.port, self.key)
        first.close()

    # ---- otadata and rollback ----

    def test_missing_or_corrupt_otadata(self):
        dev = self.start()
        self.assertEqual(dev.cmd("erase-otadata")["running"], "0")
        st = dev.cmd("boot")  # no valid entry: the first bootable slot, written back as the active one
        self.assertEqual((st["running"], st["boot"]), ("0", "fallback"))
        self.assertTrue(st["ota0"].startswith("valid:1"))
        b = make_image(b"build-B")
        self.assertEqual(self.push("upload", b, reboot=True)[0], 0)
        dev.cmd("confirm")
        st = dev.cmd("corrupt-otadata 1")  # the entry that selects app1 (its CRC fails)
        st = dev.cmd("boot")
        self.assertEqual(st["running"], "0")  # the other, still valid entry wins
        self.nvs_intact()

    def test_initial_otadata_copy0_corrupt(self):
        """The panel's otadata as it is now (boot_app0: copy 0 seq 1, copy 1 seq 0) with copy 0
        damaged after the migration: copy 1's seq 0 points at the empty app1; the bootloader
        must fall back to app0."""
        dev = self.start()
        dev.cmd("corrupt-otadata 0")
        st = dev.cmd("boot")
        self.assertEqual((st["running"], st["boot"]), ("0", "fallback"))
        self.nvs_intact()

    def test_crash_before_confirmation_rolls_back(self):
        dev = self.start()
        self.assertEqual(self.push("upload", make_image(b"crashy"), reboot=True)[0], 0)
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("1", "pending"))
        st = dev.cmd("crash")  # watchdog, panic or power loss before the health check passed
        self.assertEqual(st["running"], "0")
        self.assertEqual(st["ota1"].split(":")[2], "4")  # ESP_OTA_IMG_ABORTED
        self.assertEqual(dev.cmd("boot")["running"], "0")  # and it stays there
        self.nvs_intact()

    def test_failed_health_check_rolls_back(self):
        dev = self.start()
        self.assertEqual(self.push("upload", make_image(b"unhealthy"), reboot=True)[0], 0)
        st = dev.cmd("fail-health")  # e.g. no Wi-Fi within the limit: mark invalid, reboot
        self.assertEqual(st["running"], "0")
        self.assertEqual(st["ota1"].split(":")[2], "3")  # ESP_OTA_IMG_INVALID
        self.nvs_intact()

    def test_damaged_selected_image_falls_back(self):
        dev = self.start()
        self.assertEqual(self.push("upload", make_image(b"build-B"), reboot=True)[0], 0)
        dev.cmd("confirm")
        dev.cmd("corrupt-slot 1")  # flash damage after the switch
        st = dev.cmd("boot")
        self.assertEqual((st["running"], st["boot"]), ("0", "fallback"))
        self.nvs_intact()

    def test_real_build_image(self):
        """The real ESP32 build (build/esp32-ab) through the simulator: its format, IDF version
        and marker pass the same checks; A -> B -> A with real images."""
        path = os.path.join(HERE, "build", "esp32-ab", "x11_esp32.ino.bin")
        if not os.path.exists(path):
            self.skipTest("build the A/B image first (esp32/build_esp32.sh)")
        with open(path, "rb") as f:
            real = f.read()
        dev = self.start(app0=real)
        self.assertEqual(dev.state["running"], "0")
        st, text = self.push("upload", real, reboot=True)
        self.assertEqual(st, 0, text)
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("1", "pending"))
        dev.cmd("confirm")
        st, text = self.push("upload", real, reboot=True)
        self.assertEqual(st, 0, text)
        self.assertEqual(dev.state["running"], "0")
        self.nvs_intact()

    def test_intermediate_state_new_app_old_table(self):
        """Migration stopped after step 1 (app written, table not): the real A/B build in app0 with
        the old single-slot table boots and answers; uploads are refused (no inactive slot), so
        nothing else can change until step 2 or a revert."""
        path = os.path.join(HERE, "build", "esp32-ab", "x11_esp32.ino.bin")
        if not os.path.exists(path):
            self.skipTest("build the A/B image first (esp32/build_esp32.sh)")
        with open(path, "rb") as f:
            real = f.read()
        dev = self.start(HUGE_APP, app0=real)
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("0", "undefined"))
        st, text = self.push("status")
        self.assertEqual(st, 0)
        with self.assertRaisesRegex(ota_push.OtaError, "no inactive slot"):
            self.push("upload", real)
        self.nvs_intact()

    # ---- WLOTA2: blocks, timing, failures ----

    def test_block_sizes(self):
        for block in (4096, 16384):
            with self.subTest(block=block):
                if self.dev:
                    self.dev.stop()
                dev = self.start()
                img = make_image(b"blocks-%d" % block, size=150_000)
                timings = {}
                st, text = self.push("upload", img, reboot=True, block=block, timings=timings)
                self.assertEqual(st, 0, text)
                self.assertEqual(timings["blocks"], -(-len(img) // block))
                self.assertEqual(dev.state["running"], "1")
                self.assertEqual(int(dev.state["erases"]), -(-len(img) // 4096))  # every sector erased exactly once
        st, text = ota_push.run("127.0.0.1", self.dev.port, self.key, "status", progress=False)
        self.assertEqual(st, 0)

    def test_unsupported_block_size(self):
        self.start()
        b = make_image(b"b")
        info = ota_push.image_info(b)
        s = self.raw(1, info["size"], info["sha256"], idf=info["idf"], block=8192)
        st, _, text = s.reply()
        self.assertEqual((ota_push.STATUS[st], text), ("bad request", "unsupported block size"))
        s.close()

    def test_slow_network_and_slow_flash(self):
        """512-byte reads with a pause per pass (slow TCP), 6 ms erase and 2 ms program per
        sector: the upload completes, the device reports the time spent in each phase, and
        every sector is erased exactly once."""
        dev = self.start(extra=("--rx-chunk", "512", "--rx-delay-ms", "1", "--erase-ms", "6", "--program-ms", "2"))
        img = make_image(b"slow", size=120_000)
        timings = {}
        st, text = self.push("upload", img, reboot=True, block=16384, timings=timings)
        self.assertEqual(st, 0, text)
        sectors = -(-len(img) // 4096)
        self.assertEqual(int(dev.state["erases"]), sectors)
        self.assertGreaterEqual(timings["erase_us"], sectors * 6000)
        self.assertGreaterEqual(timings["program_us"], sectors * 2000)
        self.assertGreater(timings["recv_us"], 0)
        st = dev.stats()
        self.assertEqual((st["ota.up.size"], st["ota.up.committed"], st["ota.up.sectors"]), (len(img), len(img), sectors))
        self.assertEqual(st["ota.up.blocks"], -(-len(img) // 16384))
        self.assertGreaterEqual(st["ota.up.erase_ms"], sectors * 6)
        self.assertGreaterEqual(st["ota.up.erase_us_max"], 6000)
        self.assertGreater(st["ota.up.rate_bytes_per_s"], 0)
        self.nvs_intact()

    def test_duplicate_block(self):
        dev = self.start()
        b = make_image(b"dup")
        info = ota_push.image_info(b)
        s = self.raw(1, info["size"], info["sha256"], idf=info["idf"])
        self.assertEqual(s.reply()[0], 0)
        block0 = b"WLB2" + struct.pack("<III", 0, 0, 16384) + b[:16384]
        s.s.sendall(block0)
        magic, st, _ = s.message()
        self.assertEqual((magic, st), (b"WLOTA2AK", 0))
        s.s.sendall(block0)  # the same block again
        st, _, text = s.reply()
        self.assertEqual((ota_push.STATUS[st], text), ("bad request", "duplicate block"))
        s.close()
        self.assertEqual(dev.cmd("boot")["running"], "0")  # nothing selected
        self.nvs_intact()

    def test_flash_write_failure(self):
        dev = self.start(extra=("--fail-write-at", "0x8000"))
        st, text = self.push("upload", make_image(b"badflash"))
        self.assertEqual(ota_push.STATUS[st], "flash error")
        self.assertEqual(dev.cmd("boot")["running"], "0")
        self.assertEqual(dev.state["entry"], "undefined")  # otadata untouched
        self.nvs_intact()

    def test_tampered_ack_stops_the_client(self):
        """A man in the middle changes the first ACK (here its committed offset, which breaks its
        MAC): the client stops before sending another block, the device aborts, nothing is selected."""
        dev = self.start()
        lsock = socket.socket()
        lsock.bind(("127.0.0.1", 0))
        lsock.listen(1)
        proxy_port = lsock.getsockname()[1]

        def proxy():
            c, _ = lsock.accept()
            d = socket.create_connection(("127.0.0.1", dev.port))
            seen = bytearray()

            def down():  # device -> client: tamper with the third message (the first ACK)
                count = 0
                buf = b""
                while True:
                    try:
                        data = d.recv(65536)
                    except OSError:
                        break
                    if not data:
                        break
                    buf += data
                    while len(buf) >= (64 if count == 0 else 96):
                        n = 64 if count == 0 else 96
                        m, buf = bytearray(buf[:n]), buf[n:]
                        if count == 2:
                            m[16] ^= 1
                        try:
                            c.sendall(bytes(m))
                        except OSError:
                            return
                        count += 1
                c.close()

            t = threading.Thread(target=down, daemon=True)
            t.start()
            while True:
                try:
                    data = c.recv(65536)
                    if not data:
                        break
                    seen.extend(data)
                    d.sendall(data)
                except OSError:
                    break
            d.close()

        threading.Thread(target=proxy, daemon=True).start()
        path = os.path.join(self.tmp, "image.bin")
        with open(path, "wb") as f:
            f.write(make_image(b"mitm", size=100_000))
        with self.assertRaisesRegex(ota_push.OtaError, "does not authenticate"):
            ota_push.run("127.0.0.1", proxy_port, self.key, "upload", path, progress=False)
        time.sleep(0.5)
        self.assertEqual(dev.cmd("boot")["running"], "0")
        self.assertEqual(dev.state["entry"], "undefined")
        lsock.close()
        self.nvs_intact()

    def test_protocol_1_clients_cannot_speak_to_it(self):
        dev = self.start()
        # a hand-made WLOTA1 request: refused as a bad request, before any MAC check
        s = ota_push.Session("127.0.0.1", dev.port, self.key)
        body = (b"WLOTA1RQ" + bytes([2, 0, 0, 0]) + s.client_nonce + bytes(4 + 32 + 32 + 32 + 32))
        s.s.sendall(body + hmac.new(self.key, s.server_nonce + body, hashlib.sha256).digest())
        st, detail, text = s.reply()
        self.assertEqual((ota_push.STATUS[st], detail), ("bad request", 1))
        s.close()
        # the protocol-1 tool (commit d08fca6) stops at the HELLO, sends nothing
        old = subprocess.run(["git", "show", "d08fca6:experiments/x11-esp32/tools/ota_push.py"], capture_output=True,
                             text=True, cwd=HERE)
        if old.returncode != 0:
            self.skipTest("git history not available")
        old_tool = os.path.join(self.tmp, "ota_push_v1.py")
        with open(old_tool, "w") as f:
            f.write(old.stdout)
        img = os.path.join(self.tmp, "img.bin")
        with open(img, "wb") as f:
            f.write(make_image(b"old"))
        r = subprocess.run([sys.executable, old_tool, "--host", "127.0.0.1", "--port", str(dev.port), "--key-file",
                            dev.key_path, "upload", img], capture_output=True, text=True, timeout=30)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not a WLOTA1 endpoint", r.stderr)
        self.assertEqual(dev.cmd("boot")["running"], "0")
        self.nvs_intact()

    def test_boot_other(self):
        dev = self.start()
        self.assertEqual(self.push("upload", make_image(b"build-B"), reboot=True)[0], 0)
        dev.cmd("confirm")
        st, _ = self.push("boot-other")
        self.assertEqual(st, 0)
        self.assertEqual(dev.next_state()["running"], "0")  # back on A, on trial
        self.assertEqual(dev.cmd("confirm")["entry"], "valid")
        self.nvs_intact()

    # ---- the 8-bit message position (hardware, 2026-11-04: a 4 KiB upload of the real build
    # stopped at block 256: the device's index wrapped to 0, the client refused it) ----

    def record_indices(self):
        """Patches Session.check to record the position byte of every device message."""
        seen, orig = [], ota_push.Session.check

        def check(s, r):
            out = orig(s, r)
            seen.append(r[63])
            return out
        ota_push.Session.check = check
        self.addCleanup(setattr, ota_push.Session, "check", orig)
        return seen

    def app_region(self, slot: int) -> bytes:
        off = 0x10000 + slot * 0x300000
        return self.dev.flash()[off:off + 0x300000]

    def test_current_firmware_size_in_16k_blocks(self):
        """An image of the size of the build now on the panel (1,062,464 bytes): 65 blocks,
        67 signed messages numbered 0..66 without repetition; app0 untouched."""
        dev = self.start()
        app0 = self.app_region(0)
        img = image_of_size(b"current-size", CURRENT_SIZE)
        seen = self.record_indices()
        st, text = self.push("upload", img, reboot=True, block=16384)
        self.assertEqual(st, 0, text)
        self.assertEqual(seen[:67], list(range(67)))  # then 1 more: the reboot session's own index 0
        self.assertEqual(dev.state["running"], "1")
        self.assertEqual(self.app_region(1)[:len(img)], img)
        self.assertEqual(self.app_region(0), app0)
        self.nvs_intact()

    def test_maximum_slot_size_in_16k_blocks(self):
        """The largest image the 3 MiB slot holds: 192 blocks, 194 messages: still inside 0..255."""
        dev = self.start()
        img = image_of_size(b"max-size", 0x300000)
        seen = self.record_indices()
        st, text = self.push("upload", img, block=16384)
        self.assertEqual(st, 0, text)
        self.assertEqual(seen, list(range(194)))
        self.assertEqual(self.app_region(1), img)
        st = dev.stats()
        self.assertEqual((st["ota.up.blocks"], st["ota.up.sectors"]), (192, 768))  # each sector erased once
        self.assertEqual(dev.cmd("boot")["running"], "1")
        self.nvs_intact()

    def test_4k_current_size_refused_before_connecting(self):
        dev = self.start()
        before = dev.flash()
        img = image_of_size(b"current-size", CURRENT_SIZE)
        with self.assertRaisesRegex(ota_push.OtaError, r"262 signed device messages.*--block 16384"):
            self.push("upload", img, block=4096)
        self.assertEqual(dev.stats()["ota.up.size"], 0)  # no upload ever started
        self.assertEqual(dev.flash(), before)

    def test_4k_boundary_256_messages(self):
        """254 blocks of 4 KiB: exactly 256 messages, the last numbered 255; one block more is refused (images are multiples of 16 bytes)."""
        dev = self.start()
        img = image_of_size(b"edge", 254 * 4096)
        seen = self.record_indices()
        st, text = self.push("upload", img, block=4096)
        self.assertEqual(st, 0, text)
        self.assertEqual(seen, list(range(256)))
        self.assertEqual(self.app_region(1)[:len(img)], img)
        with self.assertRaisesRegex(ota_push.OtaError, "257 signed device messages"):
            self.push("upload", image_of_size(b"edge+16", 254 * 4096 + 16), block=4096)
        self.nvs_intact()

    def test_partial_app1_from_aborted_4k_upload_is_overwritten(self):
        """The panel's state now: app1 holds 256 sectors of an aborted 4 KiB upload. Replayed here
        on the wire (the client stops at the 257th message, as on the hardware); nothing is
        selected, app0 and otadata do not change; the next 16 KiB session erases and rewrites
        every sector it needs from offset 0, and the result is exactly the new image, whether it
        is shorter or as long as the partial one."""
        for new_size in (300_000, CURRENT_SIZE):
            with self.subTest(new_size=new_size):
                if self.dev:
                    self.dev.stop()
                dev = self.start()
                otadata0 = dev.flash()[OTADATA:OTADATA + 0x2000]
                app0 = self.app_region(0)
                old = image_of_size(b"aborted", CURRENT_SIZE)
                info = ota_push.image_info(old)
                s = self.raw(1, info["size"], info["sha256"], idf=info["idf"], block=4096)
                self.assertEqual(s.reply()[0], 0)
                with self.assertRaisesRegex(ota_push.OtaError, "position would repeat"):
                    for seq in range(260):
                        off = seq * 4096
                        s.s.sendall(b"WLB2" + struct.pack("<III", seq, off, 4096) + old[off:off + 4096])
                        magic, st, _ = s.message()
                        self.assertEqual((magic, st), (b"WLOTA2AK", 0))
                self.assertEqual(seq, 255)  # the 256th block's ACK was the one refused
                s.close()
                time.sleep(0.3)
                self.assertEqual(self.app_region(1)[:256 * 4096], old[:256 * 4096])  # the partial data is there
                self.assertEqual(dev.cmd("boot")["running"], "0")  # nothing selected
                self.assertEqual(dev.flash()[OTADATA:OTADATA + 0x2000], otadata0)
                self.assertEqual(self.app_region(0), app0)
                new = image_of_size(b"fresh", new_size)
                st, text = self.push("upload", new, reboot=True, block=16384)
                self.assertEqual(st, 0, text)
                self.assertEqual(dev.state["running"], "1")
                self.assertEqual(self.app_region(1)[:len(new)], new)
                self.assertEqual(self.app_region(0), app0)
                self.nvs_intact()


    # ---- M4: rollback of an unconfirmed trial ----
    # The panel before M4: app0 = the current image, VALID, running; app1 = the previous one,
    # VALID. The test image never confirms itself (X11_OTA_TEST_NO_CONFIRM): in the simulator
    # that is simply never sending "confirm".

    STATE = {0: "NEW", 1: "PENDING_VERIFY", 2: "VALID", 3: "INVALID", 4: "ABORTED", 255: "UNDEFINED"}

    def slot_states(self):
        """{slot: state name} from the simulator's otadata copies (slot = (seq - 1) % 2)."""
        out = {}
        best = {}
        for k in ("ota0", "ota1"):
            valid, seq, state = self.dev.state[k].split(":")
            seq, state = int(seq), int(state)
            if seq == 0:
                continue
            slot = (seq - 1) % 2
            if slot not in best or seq > best[slot]:
                best[slot], out[slot] = seq, self.STATE[state]
        return out

    def m4_start(self):
        """Both slots VALID, app0 running (the panel's state)."""
        prev, cur = make_image(b"previous"), make_image(b"current")
        dev = self.start(app0=prev)
        self.assertEqual(self.push("upload", prev, reboot=True)[0], 0)   # -> app1
        dev.cmd("confirm")
        self.assertEqual(self.push("upload", cur, reboot=True)[0], 0)    # -> app0
        dev.cmd("confirm")
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("0", "valid"))
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "VALID"})
        return dev

    def test_m4_controlled_reboot_aborts_the_trial(self):
        """VALID -> NEW -> PENDING_VERIFY -> (controlled reboot, no confirmation) ABORTED ->
        the previous VALID slot; the RTC-watchdog double reset does not change the outcome."""
        dev = self.m4_start()
        test_img = make_image(b"m4-no-confirm")
        st, text = self.push("upload", test_img)                 # written and selected, no reboot
        self.assertEqual(st, 0, text)
        dev.state = dev.cmd("state")
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "NEW"})
        self.assertEqual(dev.state["running"], "0")              # still the current image
        st, _ = ota_push.run("127.0.0.1", dev.port, self.key, "reboot", progress=False)
        self.assertEqual(st, 0)
        dev.state = dev.next_state()
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("1", "pending"))
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "PENDING_VERIFY"})
        # the trial runs (healthy, never confirmed); then the controlled reset: authenticated reboot
        st, _ = ota_push.run("127.0.0.1", dev.port, self.key, "reboot", progress=False)
        self.assertEqual(st, 0)
        dev.state = dev.next_state()
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("0", "valid"))
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "ABORTED"})
        # the RTC watchdog issue: a second reset right after: same slot, same states
        dev.cmd("crash")
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("0", "valid"))
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "ABORTED"})
        # the fallback keeps OTA access, and a good image can go to app1 again (the restore step)
        self.assertEqual(self.push("status")[0], 0)
        self.assertEqual(self.push("upload", make_image(b"restore"), reboot=True)[0], 0)
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("1", "pending"))
        dev.cmd("confirm")
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "VALID"})
        self.nvs_intact()

    def test_m4_deadline_marks_invalid(self):
        """The test image left alone: at the 180 s deadline it marks itself INVALID and
        reboots (esp_ota_mark_app_invalid_rollback_and_reboot) -> the previous slot."""
        dev = self.m4_start()
        self.assertEqual(self.push("upload", make_image(b"m4-no-confirm"), reboot=True)[0], 0)
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "PENDING_VERIFY"})
        dev.cmd("fail-health")
        self.assertEqual((dev.state["running"], dev.state["entry"]), ("0", "valid"))
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "INVALID"})
        dev.cmd("crash")  # a watchdog reset afterwards changes nothing
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "INVALID"})
        self.nvs_intact()

    def test_m4_unrelated_reset_of_a_confirmed_image_is_not_a_rollback(self):
        """An RTC-watchdog or software reset of the confirmed image boots it again: only a
        trial (PENDING_VERIFY) can be aborted."""
        dev = self.m4_start()
        for _ in range(3):
            dev.cmd("crash")
            self.assertEqual((dev.state["running"], dev.state["entry"]), ("0", "valid"))
            self.assertEqual(self.slot_states(), {0: "VALID", 1: "VALID"})
        self.nvs_intact()

    def test_m4_reset_before_the_reboot_command(self):
        """A trial that resets by itself (panic, task watchdog, power) before the planned
        trigger: the same ABORTED fallback; the test records it as an unplanned trigger."""
        dev = self.m4_start()
        self.assertEqual(self.push("upload", make_image(b"m4-no-confirm"), reboot=True)[0], 0)
        dev.cmd("crash")
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "ABORTED"})
        self.assertEqual(dev.state["running"], "0")
        self.nvs_intact()

    def test_m4_real_test_image(self):
        """The real no-confirm build (build/esp32-ab-m4-noconfirm) passes the receiver's checks
        and goes through the same sequence."""
        path = os.path.join(HERE, "build", "esp32-ab-m4-noconfirm", "x11_esp32.ino.bin")
        if not os.path.exists(path):
            self.skipTest("build the M4 test image first (X11_TEST_FLAGS=-DX11_OTA_TEST_NO_CONFIRM)")
        with open(path, "rb") as f:
            img = f.read()
        dev = self.m4_start()
        self.assertEqual(self.push("upload", img, reboot=True)[0], 0)
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "PENDING_VERIFY"})
        st, _ = ota_push.run("127.0.0.1", dev.port, self.key, "reboot", progress=False)
        self.assertEqual(st, 0)
        dev.state = dev.next_state()
        self.assertEqual(dev.state["running"], "0")
        self.assertEqual(self.slot_states(), {0: "VALID", 1: "ABORTED"})


CURRENT_SIZE = 1_062_464  # the build on the panel (app sha256 7602ccfd...)


def image_of_size(tag: bytes, total: int) -> bytes:
    """make_image() with an exact total length (its padding varies by up to 15 bytes)."""
    over = len(make_image(tag, size=1000)) - 1000
    for body in range(total - over + 16, total - over - 32, -1):
        img = make_image(tag, size=body)
        if len(img) == total:
            return img
    raise AssertionError(f"no image of exactly {total} bytes")


class MessageBudgetTest(unittest.TestCase):
    """The client-side guard for the WLOTA2 one-byte message position (no device needed)."""

    def test_counts(self):
        f = ota_push.signed_messages
        self.assertEqual(f(CURRENT_SIZE, 16384), 67)   # 65 blocks
        self.assertEqual(f(CURRENT_SIZE, 4096), 262)   # 260 blocks: what wrapped on the panel
        self.assertEqual(f(0x300000, 16384), 194)      # the largest image app1 holds
        self.assertEqual(f(0x300000, 4096), 770)
        self.assertEqual(f(254 * 4096, 4096), 256)
        self.assertEqual(f(254 * 4096 + 1, 4096), 257)
        self.assertEqual(f(1, 16384), 3)

    def test_guard(self):
        g = ota_push.check_message_budget
        self.assertEqual(g(CURRENT_SIZE, 16384), 67)
        self.assertEqual(g(0x300000, 16384), 194)
        self.assertEqual(g(254 * 4096, 4096), 256)
        for size, block in ((CURRENT_SIZE, 4096), (0x300000, 4096), (254 * 4096 + 1, 4096)):
            with self.assertRaises(ota_push.OtaError):
                g(size, block)
        # every image that fits a 3 MiB slot fits the budget in 16 KiB blocks
        self.assertLessEqual(ota_push.signed_messages(0x300000, max(ota_push.BLOCK_SIZES)), ota_push.MAX_SIGNED_MESSAGES)

    def session(self, replies: int):
        s = object.__new__(ota_push.Session)
        s.key, s.client_nonce, s.server_nonce, s.replies = os.urandom(32), os.urandom(16), os.urandom(16), replies
        return s

    def signed(self, s, index: int) -> bytes:
        body = bytearray(64)
        body[:8] = b"WLOTA2AK"
        body[63] = index
        return bytes(body) + hmac.new(s.key, s.client_nonce + s.server_nonce + bytes(body), hashlib.sha256).digest()

    def test_check_never_accepts_a_257th_message(self):
        s = self.session(255)
        self.assertEqual(s.check(self.signed(s, 255))[0], b"WLOTA2AK")  # the 256th: index 255, accepted
        with self.assertRaisesRegex(ota_push.OtaError, "position would repeat"):
            s.check(self.signed(s, 0))  # the 257th would carry 0 again: authentic, but refused
        t = self.session(3)
        with self.assertRaisesRegex(ota_push.OtaError, "out of order"):
            t.check(self.signed(t, 4))


if __name__ == "__main__":
    unittest.main()
