"""Host server end to end for the last core requests, through real sockets and the transport
(net/x11_net.c), both byte orders: resources kept after a RetainPermanent client's socket
closes, KillClient of a retained client and of a live one (its socket is closed), the panel's
no-reset default, and host access answered BadAccess with the cookie still required."""
import os
import socket
import struct
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_host import COOKIE, SERVER, Server  # noqa: E402
from x11_client import connect, recv_exact  # noqa: E402


class C:
    """A set-up connection: requests with a GetInputFocus barrier, answers as tuples."""

    def __init__(self, port, msb, cookie=None):
        self.e = ">" if msb else "<"
        self.s, rep = connect(port, msb, *((b"MIT-MAGIC-COOKIE-1", cookie) if cookie else ()))
        assert rep["status"] == "success", rep
        self.base, self.root, self.seq, self.n = rep["resource_id_base"], rep["screens"][0]["root"], 0, 0

    def req(self, op, d1=0, body=b"", reply=False):
        body += b"\0" * (-len(body) % 4)
        self.s.sendall(struct.pack(self.e + "BBH", op, d1, 1 + len(body) // 4) + body)
        self.seq += 1
        want = self.seq
        if not reply:
            self.s.sendall(struct.pack(self.e + "BBH", 43, 0, 1))
            self.seq += 1
        while True:
            m = recv_exact(self.s, 32)
            seq = struct.unpack_from(self.e + "H", m, 2)[0]
            if m[0] == 0 and seq == want:
                if not reply:
                    recv_exact(self.s, 32)  # the barrier's reply
                return ("error", m[1], struct.unpack_from(self.e + "I", m, 4)[0])
            if m[0] == 1:
                m += recv_exact(self.s, 4 * struct.unpack_from(self.e + "I", m, 4)[0])
                if seq == want:
                    return ("reply", m)
                if not reply and seq == self.seq:
                    return ("ok",)

    def pixmap(self):
        self.n += 1
        pid = self.base + self.n
        assert self.req(53, 16, struct.pack(self.e + "IIHH", pid, self.root, 4, 4)) == ("ok",)
        return pid

    def geometry(self, d):
        return self.req(14, 0, struct.pack(self.e + "I", d), reply=True)[0:2]

    def kill(self, rid):
        return self.req(113, 0, struct.pack(self.e + "I", rid))

    def closed(self, wait=2.0):
        self.s.settimeout(wait)
        try:
            return self.s.recv(1) == b""
        except socket.timeout:
            return False


@unittest.skipUnless(os.path.exists(SERVER), "build the server first (make)")
class CloseDownHostTest(unittest.TestCase):
    def setUp(self):
        self.srv = None

    def tearDown(self):
        if self.srv and self.srv.p.poll() is None:
            self.srv.log()

    def test_retained_and_killed(self):
        self.srv = Server("--clients", "8")
        for msb in (False, True):
            b = C(self.srv.port, msb)
            a = C(self.srv.port, not msb)
            self.assertEqual(a.req(112, 1), ("ok",))                     # RetainPermanent
            pa = a.pixmap()
            a.s.close()
            time.sleep(0.3)
            self.assertEqual(b.geometry(pa)[0], "reply")                 # kept after the socket closed
            self.assertEqual(b.kill(pa), ("ok",))
            self.assertEqual(b.geometry(pa), ("error", 9))               # BadDrawable
            self.assertEqual(b.kill(pa)[:2], ("error", 2))               # BadValue
            # a live client in RetainTemporary mode, killed: socket closed, resources kept
            l = C(self.srv.port, msb)
            l.req(112, 2)
            pl = l.pixmap()
            self.assertEqual(b.kill(pl), ("ok",))
            self.assertTrue(l.closed())
            self.assertEqual(b.geometry(pl)[0], "reply")
            self.assertEqual(b.kill(0), ("ok",))                         # AllTemporary
            self.assertEqual(b.geometry(pl), ("error", 9))
            # Destroy mode (the default): the pixmap goes with the socket
            d = C(self.srv.port, msb)
            pd = d.pixmap()
            d.s.close()
            time.sleep(0.3)
            self.assertEqual(b.geometry(pd), ("error", 9))
            # the panel default: no reset, so the last connection closing keeps retained clients
            r = C(self.srv.port, msb)
            r.req(112, 1)
            pr = r.pixmap()
            r.s.close()
            b.s.close()
            time.sleep(0.3)
            c = C(self.srv.port, msb)
            self.assertEqual(c.geometry(pr)[0], "reply")
            self.assertEqual(c.kill(pr), ("ok",))
            c.s.close()
            time.sleep(0.2)

    def test_retained_clients_hold_slots_at_64(self):
        """64 slots: 4 retained clients (the cap) and 60 live ones fill them; the 65th setup is
        refused until KillClient frees a retained slot; a 5th Retain-mode close is destroyed."""
        self.srv = Server("--clients", "64", "-q")
        port = self.srv.port
        live = [C(port, i & 1) for i in range(55)]
        kept = []
        for i in range(5):
            r = C(port, i & 1)
            r.req(112, 1)
            kept.append(r.pixmap())
            r.s.close()
            time.sleep(0.15)
        q = live[0]
        present = [q.geometry(p)[0] for p in kept]
        self.assertEqual(present, ["reply"] * 4 + ["error"])          # the cap: the 5th was destroyed
        live += [C(port, False) for _ in range(5)]                     # 60 live + 4 retained = 64
        s, rep = connect(port, False)
        s.close()
        self.assertEqual(rep["status"], "failed")                      # no free slot
        self.assertEqual(q.kill(kept[0]), ("ok",))                     # frees a retained slot
        extra = C(port, True)
        self.assertEqual(extra.geometry(kept[1])[0], "reply")          # the other retained remain
        for c in live + [extra]:
            c.s.close()

    def test_access_control_keeps_the_cookie(self):
        self.srv = Server("--cookie", COOKIE.hex())
        for msb in (False, True):
            a = C(self.srv.port, msb, COOKIE)
            r = a.req(110, reply=True)                                   # ListHosts
            self.assertEqual((r[0], r[1][1], struct.unpack_from(a.e + "H", r[1], 8)[0]), ("reply", 1, 0))
            self.assertEqual(a.req(111, 0)[:2], ("error", 10))           # SetAccessControl Disable: BadAccess
            self.assertEqual(a.req(109, 0, struct.pack(a.e + "BBH", 0, 0, 4) + bytes([127, 0, 0, 1]))[:2],
                             ("error", 10))                              # ChangeHosts Insert: BadAccess
            s, rep = connect(self.srv.port, msb)                         # still no entry without the cookie
            s.close()
            self.assertEqual(rep["status"], "failed")
            a.s.close()


if __name__ == "__main__":
    unittest.main()
