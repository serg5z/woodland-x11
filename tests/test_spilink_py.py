"""The Python codec (tools/spilink/spilink.py) against frames built by the C link (spilink/)."""
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "tools", "spilink"))
import spilink  # noqa: E402

VEC = os.path.join(HERE, "build", "spilink_vectors")


@unittest.skipUnless(os.path.exists(VEC), "make build/spilink_vectors")
class CrossCheck(unittest.TestCase):
    def setUp(self):
        out = subprocess.run([VEC], capture_output=True, text=True, check=True).stdout.split()
        self.frames = [bytes.fromhex(h) for h in out]

    def test_c_frames_parse(self):
        empty, data, nak = (spilink.parse(f) for f in self.frames)
        self.assertEqual((empty["len"], empty["epoch"], empty["ack"], empty["base"]), (0, 0x5A, 0xFFFF, 0))
        self.assertEqual(data["messages"], [b"hello", b"Woodland", b"x"])
        self.assertEqual((data["seq"], data["base"]), (0, 0))
        self.assertTrue(nak["flags"] & spilink.F_NAK)

    def test_python_builds_identical_frames(self):
        p = spilink.parse(self.frames[1])
        again = spilink.build(512, flags=p["flags"], seq=p["seq"], ack=p["ack"], credit=p["credit"],
                              epoch=p["epoch"], base=p["base"], messages=p["messages"])
        self.assertEqual(again, self.frames[1])

    def test_rejects(self):
        f = bytearray(self.frames[1])
        f[100] ^= 1
        self.assertIsNone(spilink.parse(bytes(f)))
        self.assertIsNone(spilink.parse(bytes(512)))
        self.assertIsNone(spilink.parse(b"XS" + self.frames[1][2:]))


if __name__ == "__main__":
    unittest.main()
