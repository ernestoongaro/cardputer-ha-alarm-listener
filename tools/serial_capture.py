#!/usr/bin/env python3
"""Capture the Cardputer serial console exposed by ser2net (raw TCP) to a file.

Usage: serial_capture.py HOST PORT OUTFILE

Each line is prefixed with a local timestamp. Uses TCP keepalive so the stream
does not stall the way `nc` tends to with ser2net.
"""
import socket
import sys
import time

host, port, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
sock = socket.create_connection((host, port), timeout=None)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
buf = b""
with open(out, "ab", buffering=0) as f:
    while True:
        data = sock.recv(4096)
        if not data:
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            f.write(time.strftime("%H:%M:%S ").encode() + line.strip(b"\r") + b"\n")
