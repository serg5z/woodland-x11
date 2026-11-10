"""Host-side codec for the Woodland SPI link (spilink/spilink.h): frame build/parse, CRC-32,
messages. Stateless; the go-back-N state machine is the C one (spilink/), used by a host tool
through a small C extension or a port later."""
from __future__ import annotations

import struct
import zlib

VERSION = 1
HDR = 16
TRAILER = 4
F_NAK, F_RESYNC = 0x01, 0x02


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF  # IEEE 802.3, the same as spl_crc32


def build(frame_size: int, *, flags=0, seq=0, ack=0xFFFF, credit=0, epoch=0, base=0, messages=()) -> bytes:
    payload = b"".join(struct.pack("<H", len(m)) + m for m in messages)
    if frame_size % 4 or len(payload) > frame_size - HDR - TRAILER:
        raise ValueError("frame size or payload")
    head = b"WS" + bytes([VERSION, flags]) + struct.pack("<HHBBHH", seq, ack, credit, epoch, len(payload), base) + b"\0\0"
    body = (head + payload).ljust(frame_size - TRAILER, b"\0")
    return body + struct.pack("<I", crc32(body))


def parse(frame: bytes) -> dict | None:
    """The frame's fields and messages, or None if it is not acceptable (magic, version,
    length, CRC, or a malformed payload)."""
    fs = len(frame)
    if fs < HDR + TRAILER or frame[:2] != b"WS" or frame[2] != VERSION:
        return None
    seq, ack, credit, epoch, length, base = struct.unpack_from("<HHBBHH", frame, 4)
    if length > fs - HDR - TRAILER or struct.unpack_from("<I", frame, fs - 4)[0] != crc32(frame[:fs - 4]):
        return None
    msgs, o, p = [], 0, frame[HDR:HDR + length]
    while o < length:
        if o + 2 > length:
            return None
        n = struct.unpack_from("<H", p, o)[0]
        if not n or o + 2 + n > length:
            return None
        msgs.append(p[o + 2:o + 2 + n])
        o += 2 + n
    return dict(flags=frame[3], seq=seq, ack=ack, credit=credit, epoch=epoch, len=length, base=base, messages=msgs)
