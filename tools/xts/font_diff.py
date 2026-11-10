#!/usr/bin/env python3
"""Differential font check: QueryFont, ListFontsWithInfo and QueryTextExtents of every font
in a fonts.dir, on a reference server (Xvfb) and on the Woodland host server.

    font_diff.py FONTDIR REF_DISPLAY_PORT TEST_DISPLAY_PORT [--fontpath DIR]

--fontpath is set on the reference server first (on this connection, which stays open, so
Xvfb's reset on last close cannot undo it). Atoms (property names and atom values) are
compared by name. Prints one line per difference and a summary; exit status 1 if any."""
import socket
import struct
import sys


class X:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.s.sendall(b"l\0\x0b\0\0\0\0\0\0\0\0\0")
        head = self._recv(8)
        assert head[0] == 1, "setup failed"
        body = self._recv(struct.unpack("<H", head[6:8])[0] * 4)
        self.base = struct.unpack_from("<I", body, 4)[0]
        self.next = 1
        self.atoms = {}

    def _recv(self, n):
        b = b""
        while len(b) < n:
            part = self.s.recv(n - len(b))
            if not part:
                raise ConnectionError("closed")
            b += part
        return b

    def req(self, op, d1, body=b"", replies=1):
        """Sends a request; returns its reply (a list for ListFontsWithInfo), ("error", code),
        or None for a request without reply that succeeded (checked with GetInputFocus)."""
        body += b"\0" * (-len(body) % 4)
        self.s.sendall(struct.pack("<BBH", op, d1, 1 + len(body) // 4) + body)
        self.seq = (getattr(self, "seq", 0) + 1) & 0xFFFF
        seq = self.seq
        if replies == 0:
            r = self.req(43, 0)
            return self.errors.pop(seq, None) if hasattr(self, "errors") else None
        out = []
        while True:
            m = self._recv(32)
            mseq = struct.unpack_from("<H", m, 2)[0]
            if m[0] == 0:
                if mseq == seq:
                    return ("error", m[1])
                self.errors = getattr(self, "errors", {})
                self.errors[mseq] = ("error", m[1])
                continue
            if m[0] != 1:
                continue  # an event
            m += self._recv(struct.unpack_from("<I", m, 4)[0] * 4)
            if mseq != seq:
                continue
            out.append(m)
            if op != 50:
                return m
            if m[1] == 0:  # ListFontsWithInfo: the last reply
                return out

    def new_id(self):
        self.next += 1
        return self.base + self.next

    def atom_name(self, a):
        """The atom's name, or None if it does not exist (yet: not cached, it may be made later)."""
        if a not in self.atoms:
            r = self.req(17, 0, struct.pack("<I", a))
            if isinstance(r, tuple):
                return None
            self.atoms[a] = r[32:32 + struct.unpack_from("<H", r, 8)[0]].decode("latin-1")
        return self.atoms[a]


def xstr(s):
    return bytes([len(s)]) + s.encode("latin-1")


def info(x, r, props_at=60):
    """Font info fields (bytes 8..59) and properties {name: value-or-atom-name}."""
    n = struct.unpack_from("<H", r, 46)[0]
    props = {}
    for i in range(n):
        a, v = struct.unpack_from("<II", r, props_at + 8 * i)
        props[x.atom_name(a)] = (v, x.atom_name(v) if 0 < v < 0x10000000 else None)
    return r[8:46] + r[48:56], props


def compare_props(name, pr, pt, diffs):
    for k in sorted(set(pr) | set(pt), key=str):
        if k not in pr or k not in pt:
            diffs.append(f"{name}: property {k} only on {'reference' if k in pr else 'woodland'}")
            continue
        (vr, ar), (vt, at) = pr[k], pt[k]
        if vr != vt and (ar is None or ar != at):
            diffs.append(f"{name}: property {k}: reference {vr} ({ar}) woodland {vt} ({at})")


def main():
    font_dir, ref_port, test_port = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    ref, test = X(ref_port), X(test_port)
    if "--fontpath" in sys.argv:
        path = sys.argv[sys.argv.index("--fontpath") + 1]
        r = ref.req(52, 0)  # GetFontPath, then append
        n, o, elems = struct.unpack_from("<H", r, 8)[0], 32, []
        for _ in range(n):
            elems.append(r[o + 1:o + 1 + r[o]].decode())
            o += 1 + r[o]
        elems.append(path)
        body = struct.pack("<HH", len(elems), 0) + b"".join(xstr(e) for e in elems)
        assert ref.req(51, 0, body, replies=0) is None, "SetFontPath failed on the reference"
    names = [line.split(None, 1)[1].strip() for line in open(font_dir + "/fonts.dir").read().splitlines()[1:] if line.strip()]
    diffs, checks = [], 0
    for name in names:
        replies = {}
        for side, x in (("ref", ref), ("test", test)):
            fid = x.new_id()
            x.req(45, 0, struct.pack("<IHH", fid, len(name), 0) + name.encode(), replies=0)
            q = x.req(47, 0, struct.pack("<I", fid))
            lf = x.req(50, 0, struct.pack("<HH", 5, len(name)) + name.encode(), replies=2)
            replies[side] = (x, fid, q, lf)
        (xr, fr, qr, lr), (xt, ft, qt, lt) = replies["ref"], replies["test"]
        if isinstance(qr, tuple) or isinstance(qt, tuple):
            diffs.append(f"{name}: QueryFont reference {qr if isinstance(qr, tuple) else 'ok'} woodland {qt if isinstance(qt, tuple) else 'ok'}")
            continue
        ir, pr = info(xr, qr)
        it, pt = info(xt, qt)
        checks += 1
        if ir != it:
            fields = ["min_bounds", "max_bounds", "min_char/max_char/default", "dir/byte1/all/ascent/descent"]
            spans = [(0, 12), (16, 28), (32, 38), (38, 46)]
            for f, (a, b) in zip(fields, spans):
                if ir[a:b] != it[a:b]:
                    diffs.append(f"{name}: QueryFont {f}: reference {ir[a:b].hex()} woodland {it[a:b].hex()}")
        compare_props(name, pr, pt, diffs)
        cr = qr[60 + 8 * struct.unpack_from("<H", qr, 46)[0]:]
        ct = qt[60 + 8 * struct.unpack_from("<H", qt, 46)[0]:]
        checks += 1
        if cr != ct:
            bad = [i for i in range(min(len(cr), len(ct)) // 12) if cr[12 * i:12 * i + 12] != ct[12 * i:12 * i + 12]]
            diffs.append(f"{name}: char infos differ ({len(cr) // 12} vs {len(ct) // 12} slots; {len(bad)} differ, first {bad[:5]})")
        # ListFontsWithInfo: the first reply's info and properties match QueryFont's
        if not isinstance(lr, tuple) and not isinstance(lt, tuple):
            checks += 1
            ilr, plr = info(xr, lr[0])
            ilt, plt = info(xt, lt[0])
            if ilr[:38] + ilr[38:] != ilt[:38] + ilt[38:]:
                diffs.append(f"{name}: ListFontsWithInfo info differs")
            compare_props(name + " (ListFontsWithInfo)", plr, plt, diffs)
        # QueryTextExtents: each slot alone, a run of the first 40, a missing character
        b1lo, b1hi = qr[49], qr[50]
        lo, hi = struct.unpack_from("<HH", qr, 40)
        chars = [(b1, b2) for b1 in range(b1lo, b1hi + 1) for b2 in range(lo, hi + 1)]
        strings = [[c] for c in chars[:300]] + [chars[:40], [(255, 255)], chars[::7][:60], []]
        for s in strings:
            body = b"".join(bytes(c) for c in s)
            odd = len(s) % 2
            body += b"\0\0" * odd
            er = xr.req(48, odd, struct.pack("<I", fr) + body)
            et = xt.req(48, odd, struct.pack("<I", ft) + body)
            checks += 1
            if isinstance(er, tuple) or isinstance(et, tuple):
                if er != et:
                    diffs.append(f"{name}: QueryTextExtents {s[:3]}: reference {er if isinstance(er, tuple) else 'reply'} woodland {et if isinstance(et, tuple) else 'reply'}")
            elif er[1] != et[1] or er[8:28] != et[8:28]:
                diffs.append(f"{name}: QueryTextExtents {s[:3]}...({len(s)}): reference {er[8:28].hex()} woodland {et[8:28].hex()}")
    for d in diffs[:200]:
        print(d)
    print(f"{len(names)} fonts, {checks} comparisons, {len(diffs)} differences")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
