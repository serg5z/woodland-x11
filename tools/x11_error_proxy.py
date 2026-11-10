#!/usr/bin/env python3
"""Logs the X11 protocol errors a server sends, for comparing this server with a
reference X server (for example Xvfb) under the same clients.

    tools/x11_error_proxy.py LISTEN_DISPLAY UNIX_SOCKET LOG [--requests]
    e.g. Xvfb :98 -nolisten tcp &  tools/x11_error_proxy.py 97 /tmp/.X11-unix/X98 errors.log

Listens on 127.0.0.1:(6000 + LISTEN_DISPLAY) only and forwards each connection to the
server's Unix socket. Requests pass through unchanged; the server's byte stream is parsed
(setup reply, then 32-byte events and errors, replies with their length) and each error
is logged as "conn seq ErrorName major bad-value". --requests also logs each request as
"conn seq > opcode word1 word2 word3" (the connection setup block, which carries the
authorization data, is skipped and never logged)."""
import os
import select
import socket
import struct
import sys
import threading

NAMES = ["?", "BadRequest", "BadValue", "BadWindow", "BadPixmap", "BadAtom", "BadCursor", "BadFont", "BadMatch",
         "BadDrawable", "BadAccess", "BadAlloc", "BadColormap", "BadGC", "BadIDChoice", "BadName", "BadLength",
         "BadImplementation"]
lock = threading.Lock()


def serve(conn_id, client, server_path, log, requests=False):
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.connect(server_path)
    order = None
    buf = b""
    setup_done = False
    cbuf, csetup, cseq = b"", False, 0
    try:
        while True:
            r, _, _ = select.select([client, srv], [], [])
            if client in r:
                d = client.recv(65536)
                if not d:
                    break
                if order is None:
                    order = ">" if d[:1] == b"B" else "<"
                srv.sendall(d)
                if requests:
                    cbuf += d
                    while True:
                        if not csetup:
                            if len(cbuf) < 12:
                                break
                            nl, dl = struct.unpack(order + "HH", cbuf[6:10])
                            total = 12 + (nl + 3) // 4 * 4 + (dl + 3) // 4 * 4
                            if len(cbuf) < total:
                                break
                            cbuf, csetup = cbuf[total:], True
                            continue
                        if len(cbuf) < 4:
                            break
                        (n,) = struct.unpack(order + "H", cbuf[2:4])
                        size = 4 * n
                        if n == 0:  # BIG-REQUESTS
                            if len(cbuf) < 8:
                                break
                            size = 4 * struct.unpack(order + "I", cbuf[4:8])[0]
                        if len(cbuf) < size:
                            break
                        cseq = (cseq + 1) & 0xFFFF
                        words = [struct.unpack(order + "I", cbuf[o:o + 4])[0] for o in (4, 8, 12) if o + 4 <= size]
                        with lock:
                            log.write("%d %d > %d %s\n" % (conn_id, cseq, cbuf[0], " ".join("0x%x" % w for w in words)))
                        cbuf = cbuf[size:]
            if srv in r:
                d = srv.recv(65536)
                if not d:
                    break
                client.sendall(d)
                buf += d
                while True:
                    if not setup_done:
                        if len(buf) < 8:
                            break
                        (n,) = struct.unpack(order + "H", buf[6:8])
                        if len(buf) < 8 + 4 * n:
                            break
                        buf, setup_done = buf[8 + 4 * n:], True
                        continue
                    if len(buf) < 32:
                        break
                    if buf[0] == 1:
                        (n,) = struct.unpack(order + "I", buf[4:8])
                        if len(buf) < 32 + 4 * n:
                            break
                        buf = buf[32 + 4 * n:]
                    else:
                        if buf[0] == 0:
                            seq, bad = struct.unpack(order + "HI", buf[2:8])
                            name = NAMES[buf[1]] if buf[1] < len(NAMES) else "error%d" % buf[1]
                            with lock:
                                log.write("%d %d %s %d 0x%x\n" % (conn_id, seq, name, buf[10], bad))
                                log.flush()
                        buf = buf[32:]
    finally:
        client.close()
        srv.close()


def main():
    display, path, log = int(sys.argv[1]), sys.argv[2], open(sys.argv[3], "a")
    requests = "--requests" in sys.argv[4:]
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", 6000 + display))
    ls.listen(8)
    n = 0
    while True:
        c, _ = ls.accept()
        n += 1
        threading.Thread(target=serve, args=(n, c, path, log, requests), daemon=True).start()


if __name__ == "__main__":
    main()
