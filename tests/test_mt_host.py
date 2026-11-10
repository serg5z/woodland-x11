"""Simulated GT9xx multi-touch frames through the host server (x11_host_server --input, "mt"
lines): the chain the test firmware runs (mt/x11_touchchain) feeding the real X server. An X
client counts the ButtonPress/ButtonRelease events it receives; _WOODLAND_DIAG shows the
tracker and the transitions. One finger is Button1 as before; two-finger scrolls and taps are
Button4/5 and Button3; three to five fingers are tracked and never press anything."""
import os
import struct
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_touch import SERVER, TouchServer  # noqa: E402
from x11_raw import ROOT, Raw  # noqa: E402

M_PRESS, M_RELEASE = 1 << 2, 1 << 3


class MtServer(TouchServer):
    def frame(self, *contacts, count=None):
        """One report 15 ms after the previous: contacts as (id, x, y)."""
        self.t += 15
        n = len(contacts) if count is None else count
        flat = " ".join(f"{i} {x} {y}" for i, x, y in contacts)
        a = self.command(f"mt {self.t} {n} {flat}".rstrip())
        time.sleep(0.005)
        return a

    def idle(self, frames=8):
        for _ in range(frames):
            self.frame()


@unittest.skipUnless(os.path.exists(SERVER), "needs build/x11_host_server")
class MultiTouchHost(unittest.TestCase):
    def setUp(self):
        self.srv = MtServer(600 + os.getpid() % 100, trace=False)
        self.x = Raw(6000 + self.srv.display)
        self.x.select(ROOT, M_PRESS | M_RELEASE)
        self.x.sync()

    def tearDown(self):
        self.x.close()
        self.srv.stop()

    def buttons(self):
        """{button: [presses, releases]} received since the last call."""
        out = {}
        for e in self.x.poll_events(0.3):
            if e[0] & 0x7F in (4, 5):
                out.setdefault(e[1], [0, 0])[(e[0] & 0x7F) - 4] += 1
        return out

    def diag(self):
        return self.x.get_property(ROOT, self.x.intern("_WOODLAND_DIAG"), length=8192).decode(errors="replace")

    def test_one_finger_is_button1(self):
        self.srv.frame((3, 300, 200))
        for i in range(6):
            self.srv.frame((3, 300 + 10 * i, 200))
        a = self.srv.frame((3, 360, 200))
        self.assertEqual((a["x"], a["y"], a["buttons"] & 0x100), (360, 200, 0x100))
        self.srv.idle()
        self.assertEqual(self.buttons(), {1: [1, 1]})

    def test_two_finger_scroll_and_tap(self):
        for i in range(12):
            self.srv.frame((1, 300, 100 + 12 * i), (2, 420, 100 + 12 * i))
        self.srv.idle()
        b = self.buttons()
        self.assertGreaterEqual(b.get(4, [0])[0], 3)
        self.assertNotIn(1, b)
        self.srv.frame((5, 300, 300), (6, 400, 300))
        self.srv.frame((5, 300, 300), (6, 400, 300))
        self.srv.idle()
        self.assertEqual(self.buttons(), {3: [1, 1]})

    def test_five_fingers_tracked_never_pressed(self):
        pts = [(i + 1, 100 + 120 * i, 240) for i in range(5)]
        for k in range(1, 6):
            self.srv.frame(*pts[:k])
        for _ in range(5):
            self.srv.frame(*reversed(pts))  # another order each time: identity by ID
        self.srv.frame(*pts[2:])
        self.srv.idle()
        self.assertEqual(self.buttons(), {})
        d = self.diag()
        self.assertIn("mt.max_seen=5\n", d)
        self.assertIn("mt.new=5\n", d)
        self.assertIn("tc.transitions=10\n", d)                    # 5 BEGIN + 5 END (the log keeps the last 8)
        self.assertRegex(d, r"tc\.ev\d=5,75,BEGIN,5,4,580,240\n")  # frame 5, 75 ms: ID 5 in slot 4
        self.assertIn("mt.malformed=0\n", d)
        a = self.srv.frame()
        self.assertEqual(a["buttons"], 0)

    def test_touch_with_pointer_mapping_and_passive_grab(self):
        """Integration (x11-integration-rc1): SetPointerMapping swaps buttons 1 and 3, so a
        one-finger touch (physical Button1) is logical Button3; a window manager passive-grabs
        Button3 on a frame, synchronous, ButtonRelease only (mwm). Five taps: each press reaches
        the window manager, ReplayPointer hands it to the application, which gets press and
        release as Button3; nothing stays down or frozen. Gesture clicks are a different layer:
        they choose physical buttons, which the same map then applies to."""
        wm, app = Raw(6000 + self.srv.display), Raw(6000 + self.srv.display)
        frame = wm.create_window(ROOT, 100, 100, 400, 300)
        win = app.create_window(frame, 0, 0, 400, 300, events=M_PRESS | M_RELEASE)
        app.map(win)
        wm.map(frame)
        seq = app.send(116, 5, bytes([3, 2, 1, 4, 5, 0, 0, 0]))          # SetPointerMapping
        self.assertEqual(app.reply(seq)[1], 0)                             # Success
        # GrabButton: owner-events 0, window, mask ButtonRelease, sync pointer, async keyboard,
        # no confine/cursor, button 3, AnyModifier
        wm.send(28, 0, struct.pack("<IHBBIIBxH", frame, M_RELEASE, 0, 1, 0, 0, 3, 0x8000))
        wm.sync()
        app.poll_events(0.1)
        wm.poll_events(0.1)
        for tap in range(5):
            for _ in range(11):  # held still 165 ms: a still finger presses after 150 ms (B6)
                self.srv.frame((1, 300, 250))
            evs = wm.poll_events(0.3)
            press = [e for e in evs if e[0] & 0x7F == 4]
            self.assertEqual([e[1] for e in press], [3], f"tap {tap}: the window manager's press")
            wm.send(35, 2, struct.pack("<I", 0))                            # AllowEvents ReplayPointer
            wm.sync()
            self.srv.idle()                                                 # finger up
            got = [(e[0] & 0x7F, e[1]) for e in app.poll_events(0.3) if e[0] & 0x7F in (4, 5)]
            self.assertEqual(got, [(4, 3), (5, 3)], f"tap {tap}: the application's press and release")
            self.assertEqual([e for e in wm.poll_events(0.1) if e[0] & 0x7F in (4, 5)], [])
            qp = app.query_pointer()
            self.assertEqual(qp["mask"] & 0x1F00, 0, f"tap {tap}: a button stayed down")
        d = self.diag()
        self.assertIn("mt.malformed=0\n", d)
        wm.close()
        app.close()

    def test_malformed_and_failed_frames(self):
        self.srv.frame((1, 100, 100), count=9)                 # a count the controller cannot send
        self.srv.frame((20, 100, 100))                         # ID above 15
        self.srv.frame((2, 900, 100))                          # outside the controller
        self.srv.frame((3, 100, 100))
        for _ in range(3):
            self.srv.command(f"mtfail {self.srv.t + 15}")
            self.srv.t += 15
        self.srv.idle()
        d = self.diag()
        self.assertIn("mt.malformed=2\n", d)
        self.assertRegex(d, r"tc\.out_of_range=[1-9]")
        self.assertIn("tc.failed_frames=3\n", d)
        self.assertRegex(d, r",LOST,3,")
        b = self.buttons()
        self.assertEqual(sum(v[0] for v in b.values()), sum(v[1] for v in b.values()))  # nothing stuck


if __name__ == "__main__":
    unittest.main()
