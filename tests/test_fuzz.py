"""Malformed-request fuzzing of the host server built with ASan/UBSan (build/x11_host_server_san).

Seeded and deterministic: several clients in both byte orders send random requests for every
opcode (0..255) with random data, lengths that are right, short, long, zero or above the
maximum, IDs of real resources (their own, other clients', freed ones), and disconnects in
the middle of a request. The server must stay up, keep serving a fresh client, and the
sanitizers must report nothing.

FUZZ_SEED and FUZZ_ROUNDS override the defaults (a longer campaign: FUZZ_ROUNDS=200000); FUZZ_LOG
keeps the server's request log."""
import os
import random
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from x11_client import connect  # noqa: E402

SERVER = os.path.join(ROOT, "build", "x11_host_server_san")
ROOT_WINDOW, DEFAULT_CMAP = 0x26, 0x20


class Client:
    def __init__(self, port, msb):
        self.msb = msb
        # a slot is free again once the server has seen the close of a client the fuzzer just
        # dropped: retry for a while; slots still taken after 3 s are a leak
        deadline = time.time() + 3
        while True:
            self.sock, setup = connect(port, msb=msb)
            if setup["status"] == "success" or time.time() > deadline:
                break
            self.sock.close()
            time.sleep(0.02)
        assert setup["status"] == "success", setup
        self.base = setup["resource_id_base"]
        self.sock.setblocking(False)
        self.ids = []

    def drain(self):
        try:
            while self.sock.recv(1 << 16):
                pass
        except (BlockingIOError, InterruptedError):
            return True
        except OSError:
            return False
        return False  # closed by the server

    def send(self, data):
        view = memoryview(data)
        deadline = time.time() + 5
        while view:
            try:
                n = self.sock.send(view)
                view = view[n:]
            except (BlockingIOError, InterruptedError):
                if not self.drain():
                    return False
                select.select([self.sock], [self.sock], [], 0.05)
                if time.time() > deadline:
                    return False
            except OSError:
                return False
        return True


# Minimum length in 4-byte units of each core request (X11 protocol, "Encoding"); requests with a
# list may be longer. Used so that most fuzzed requests pass framing and reach the handlers.
BASE_UNITS = {
    1: 8, 2: 3, 3: 2, 4: 2, 5: 2, 6: 2, 7: 4, 8: 2, 9: 2, 10: 2, 11: 2, 12: 3, 13: 2, 14: 2, 15: 2, 16: 2,
    17: 2, 18: 6, 19: 3, 20: 6, 21: 2, 22: 4, 23: 2, 24: 6, 25: 11, 26: 6, 27: 2, 28: 6, 29: 3, 30: 4,
    31: 4, 32: 2, 33: 4, 34: 3, 35: 2, 37: 1, 38: 2, 39: 4, 40: 4, 41: 6, 42: 3, 43: 1, 44: 1, 45: 3,
    46: 2, 47: 2, 48: 2, 49: 2, 50: 2, 51: 2, 52: 1, 53: 4, 54: 2, 55: 4, 56: 3, 57: 4, 58: 3, 59: 3, 60: 2,
    61: 4, 62: 7, 63: 8, 64: 3, 65: 3, 66: 3, 67: 3, 68: 3, 69: 4, 70: 3, 71: 3, 72: 6, 73: 5, 74: 4, 75: 4,
    76: 4, 77: 4, 78: 4, 79: 2, 80: 3, 81: 2, 82: 2, 83: 2, 84: 4, 85: 3, 86: 3, 87: 4, 88: 3, 89: 2, 90: 4,
    91: 2, 92: 3, 93: 8, 94: 8, 95: 2, 96: 5, 97: 3, 98: 2, 99: 1, 100: 2, 101: 2, 102: 2, 103: 1, 104: 1,
    105: 3, 106: 1, 107: 3, 108: 1, 109: 2, 110: 1, 111: 1, 112: 1, 113: 2, 114: 3, 115: 1, 116: 1, 117: 1,
    118: 1, 119: 1, 127: 1}
VARIABLE = {1, 2, 12, 16, 18, 45, 48, 49, 50, 51, 55, 56, 58, 59, 64, 65, 66, 67, 68, 69, 70, 71, 72, 74, 75,
            76, 77, 85, 88, 89, 90, 91, 92, 98, 100, 102, 114, 116, 118}


def request(rng, c, others):
    """One random request for client c: (bytes, True if it ends the connection)."""
    e = ">" if c.msb else "<"
    op = rng.choice([rng.randrange(256), rng.randrange(1, 128), rng.randrange(1, 128)])
    if op == 36:  # GrabServer would (correctly) stall the other fuzzing clients
        op = 127
    if op in BASE_UNITS and rng.random() < 0.7:  # the right size (a list request: a few more words)
        words = BASE_UNITS[op] - 1 + (rng.randrange(12) if op in VARIABLE else 0)
    else:
        words = rng.choice([0, 1, 2, 3, 4, 5, 6, 8, rng.randrange(64), rng.randrange(1024)])
    body = bytearray(rng.randbytes(words * 4))
    pool = [ROOT_WINDOW, DEFAULT_CMAP, 0, 1, 0xFFFFFFFF, c.base, c.base + 1] + c.ids
    for o in others:
        pool += o.ids[:4] + [o.base + 1]
    for off in range(0, len(body) - 3, 4):  # many fields are resource IDs: use real ones
        if rng.random() < 0.4:
            struct.pack_into(e + "I", body, off, rng.choice(pool))
        elif rng.random() < 0.2:
            struct.pack_into(e + "hh", body, off, rng.randrange(-40, 900), rng.randrange(-40, 600))
    units = 1 + words
    r = rng.random()
    if r < 0.03:
        units = 0
    elif r < 0.04:  # above the maximum: the server discards what follows, so the client ends here
        units = rng.choice([4097, 0xFFFF, rng.randrange(4097, 0x10000)])
    elif r < 0.12:
        units = max(0, units + rng.choice([-2, -1, 1, 2]))
    head = struct.pack(e + "BBH", op, rng.randrange(256), units & 0xFFFF)
    data = head + bytes(body)
    cut = rng.random() < 0.01 or units > 4096
    if cut:
        data = data[: rng.randrange(1, len(data) + 1)]
    return data, cut


def create(rng, c):
    """A valid CreateWindow, CreatePixmap, CreateGC or OpenFont with a new ID of c."""
    e = ">" if c.msb else "<"
    rid = c.base + 2 + len(c.ids)
    kind = rng.randrange(4)
    parent = rng.choice([ROOT_WINDOW] + c.ids)
    if kind == 0:
        body = struct.pack(e + "IIhhHHHHII", rid, parent, rng.randrange(-20, 700), rng.randrange(-20, 400),
                           rng.randrange(1, 300), rng.randrange(1, 300), rng.randrange(3), 1, 0, 0x2) + struct.pack(e + "I", 0x00FFFFFF)
        data = struct.pack(e + "BBH", 1, 0, 1 + len(body) // 4) + body
    elif kind == 1:
        body = struct.pack(e + "IIHH", rid, ROOT_WINDOW, rng.randrange(1, 200), rng.randrange(1, 200))
        data = struct.pack(e + "BBH", 53, rng.choice([1, 16]), 4) + body
    elif kind == 2:
        body = struct.pack(e + "III", rid, ROOT_WINDOW, 0)
        data = struct.pack(e + "BBH", 55, 0, 4) + body
    else:
        body = struct.pack(e + "IHH", rid, 5, 0) + b"fixed\0\0\0"
        data = struct.pack(e + "BBH", 45, 0, 5) + body
    c.ids.append(rid)
    return data


def wait_closed(closing, timeout):
    """Drains half-closed clients until the server has closed them (up to `timeout` s)."""
    deadline = time.time() + timeout
    while closing:
        for c in list(closing):
            if not c.drain():
                c.sock.close()
                closing.remove(c)
        if not closing or time.time() > deadline:
            break
        select.select([c.sock for c in closing], [], [], 0.05)


def alive(port):
    c = Client(port, 0)
    c.sock.setblocking(True)
    c.sock.settimeout(5)
    c.sock.sendall(struct.pack("<BBH", 43, 0, 1))  # GetInputFocus
    rep = c.sock.recv(32)
    c.sock.close()
    return len(rep) == 32 and rep[0] == 1


@unittest.skipUnless(os.path.exists(SERVER), "build the sanitizer server first (make build/x11_host_server_san)")
class Fuzz(unittest.TestCase):
    def test_malformed_requests(self):
        seed = int(os.environ.get("FUZZ_SEED", "1"))
        rounds = int(os.environ.get("FUZZ_ROUNDS", "6000"))
        display = 450 + os.getpid() % 300
        port = 6000 + display
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:halt_on_error=1",
                   UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
        # the server's log goes to a file (a pipe nobody reads would block the server when full);
        # FUZZ_LOG=PATH keeps it, with every request, for reproducing a failure
        keep = os.environ.get("FUZZ_LOG")
        log = open(keep, "w+b") if keep else tempfile.TemporaryFile()
        p = subprocess.Popen([SERVER, "-d", str(display), "--clients", "8"] + ([] if keep else ["-q"]),
                             stderr=log, env=env)
        try:
            for _ in range(50):
                log.seek(0)
                if b"listening" in log.read():
                    break
                time.sleep(0.05)
            rng = random.Random(seed)
            clients, closing = [], []
            for i in range(rounds):
                if len(clients) < 3:
                    clients.append(Client(port, rng.random() < 0.5))
                c = rng.choice(clients)
                others = [o for o in clients if o is not c]
                if rng.random() < 0.15 and len(c.ids) < 40:
                    data, cut = create(rng, c), False
                else:
                    data, cut = request(rng, c, others)
                ok = c.send(data)
                if cut or not ok or rng.random() < 0.003:
                    # half-close: the server reads everything sent, then the end (an abrupt
                    # close would reset the connection and the server would never see the data)
                    clients.remove(c)
                    try:
                        c.sock.shutdown(socket.SHUT_WR)
                        closing.append(c)
                    except OSError:
                        c.sock.close()
                if i % 16 == 0 or len(closing) > 3:
                    for o in clients:
                        o.drain()
                    wait_closed(closing, 5 if len(closing) > 3 else 0)
                if p.poll() is not None:
                    break
            for c in clients:
                c.sock.shutdown(socket.SHUT_WR)
            wait_closed(closing + clients, 30)
            self.assertIsNone(p.poll(), "server exited (seed %d)" % seed)
            self.assertTrue(alive(port), "server stopped serving (seed %d)" % seed)
        finally:
            if p.poll() is None:
                p.terminate()
            p.wait(timeout=10)
            log.seek(0)
            err = log.read().decode(errors="replace")
            log.close()
        bad = [line for line in err.splitlines() if "Sanitizer" in line or "runtime error" in line]
        self.assertEqual(bad, [], "seed %d:\n%s" % (seed, err[-4000:]))


if __name__ == "__main__":
    unittest.main()
