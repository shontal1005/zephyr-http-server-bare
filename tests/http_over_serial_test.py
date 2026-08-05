#!/usr/bin/env python3
#
# Copyright (c) 2026 Zephyr Project contributors
#
# SPDX-License-Identifier: Apache-2.0
"""
Exercise the bare HTTP server over a serial link.

Point it at whatever the board's http-uart is wired to::

    ./tests/http_over_serial_test.py /dev/ttyUSB0
    ./tests/http_over_serial_test.py /dev/ttyACM0 --baud 921600

native_sim works the same way - it prints the pseudoterminal backing uart1 on
startup ("uart_1 connected to pseudotty: /dev/pts/9"), so just pass that path::

    ./tests/http_over_serial_test.py /dev/pts/9

Checks, in order:

  1. GET  /files/hello.txt   downloads the seeded file
  2. PUT  /files/<name>      uploads a generated payload
  3. GET  /files/<name>      reads it back and compares byte for byte
  4. GET  /files/<missing>   returns 404 without dropping the connection
  5. GET  /files/hello.txt   still works, proving keep-alive across all of it

Every request rides the same connection, so a clean run also proves the server
never hung up. pyserial is used when available (needed to set a baud rate);
otherwise the device is opened raw, which is all a pseudoterminal needs.
"""

import argparse
import os
import sys
import time

DEFAULT_TIMEOUT = 5.0


class Link:
    """A byte pipe to the board, over pyserial or a raw fd."""

    def __init__(self, device, baud, want_baud):
        self._serial = None
        self._fd = None

        try:
            import serial  # noqa: PLC0415
        except ImportError:
            serial = None

        if serial is not None:
            self._serial = serial.Serial(device, baud, timeout=0)
            return

        if want_baud:
            sys.exit("error: --baud needs pyserial (pip install pyserial)")

        import termios  # noqa: PLC0415
        import tty  # noqa: PLC0415

        self._fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            # A pty slave keeps the default line discipline, which would mangle
            # an HTTP byte stream (ICRNL, OPOST, ECHO). Raw mode is mandatory.
            tty.setraw(self._fd)
        except termios.error:
            pass  # not a tty; a plain pipe or file is fine as-is

    def write(self, data):
        if self._serial is not None:
            self._serial.write(data)
            self._serial.flush()
            return

        while data:
            try:
                data = data[os.write(self._fd, data):]
            except BlockingIOError:
                # Non-blocking fd and the tty buffer is momentarily full.
                time.sleep(0.001)

    def read(self):
        """Whatever is available right now, possibly b''."""
        if self._serial is not None:
            return self._serial.read(4096)
        try:
            return os.read(self._fd, 4096)
        except BlockingIOError:
            return b""
        except OSError:
            return b""

    def close(self):
        if self._serial is not None:
            self._serial.close()
        elif self._fd is not None:
            os.close(self._fd)


def send_paced(link, data, pace, gap):
    """Write in pieces so a fast host cannot outrun the board's RX handling.

    A real UART paces itself at the baud rate and needs none of this, but a
    pseudoterminal has no rate limit at all: native_pty's interrupt-emulation
    thread runs at K_HIGHEST_THREAD_PRIO without sleeping, so it hands the
    firmware an entire burst before the feed thread is ever scheduled, and the
    RX ring overruns. Use --pace 0 to write everything in one go.
    """
    if pace <= 0:
        link.write(data)
        return

    for i in range(0, len(data), pace):
        link.write(data[i:i + pace])
        time.sleep(gap)


def read_response(link, timeout):
    """Accumulate one HTTP response, using its own framing to know when to stop."""
    buf = bytearray()
    deadline = time.time() + timeout

    while time.time() < deadline:
        chunk = link.read()
        if chunk:
            buf.extend(chunk)
        else:
            time.sleep(0.01)

        head, sep, body = bytes(buf).partition(b"\r\n\r\n")
        if not sep:
            continue

        lowered = head.lower()
        if b"transfer-encoding: chunked" in lowered:
            if body.endswith(b"0\r\n\r\n") or b"\r\n0\r\n\r\n" in body:
                break
        else:
            length = 0
            for line in lowered.split(b"\r\n"):
                if line.startswith(b"content-length:"):
                    length = int(line.split(b":", 1)[1])
                    break
            if len(body) >= length:
                break

    return bytes(buf)


def body_of(response):
    """Strip the status line, headers and any chunk framing."""
    head, _, body = response.partition(b"\r\n\r\n")

    if b"transfer-encoding: chunked" not in head.lower():
        return body

    out, rest = bytearray(), body
    while True:
        size_line, _, rest = rest.partition(b"\r\n")
        try:
            size = int(size_line.strip() or b"0", 16)
        except ValueError:
            break
        if size == 0:
            break
        out.extend(rest[:size])
        rest = rest[size + 2:]

    return bytes(out)


def status_of(response):
    first = response.split(b"\r\n", 1)[0]
    parts = first.split()
    if len(parts) >= 2 and parts[1].isdigit():
        return int(parts[1])
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("device", help="serial device the board's http-uart is on")
    parser.add_argument("--baud", type=int, default=115200,
                        help="baud rate (needs pyserial; ignored for a pty)")
    parser.add_argument("--size", type=int, default=5035,
                        help="upload payload size in bytes")
    parser.add_argument("--name", default="up.bin", help="filename to upload")
    parser.add_argument("--seeded", default="hello.txt",
                        help="a file expected to already exist under the fs root")
    parser.add_argument("--prefix", default="/files/", help="URL prefix files are served under")
    parser.add_argument("--pace", type=int, default=256,
                        help="bytes per write, 0 to disable pacing")
    parser.add_argument("--gap", type=float, default=0.005, help="seconds between paced writes")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help="seconds to wait for one response")
    args = parser.parse_args()

    want_baud = any(a.startswith("--baud") for a in sys.argv[1:])
    link = Link(args.device, args.baud, want_baud)
    failures = []

    def request(raw, label):
        send_paced(link, raw, args.pace, args.gap)
        response = read_response(link, args.timeout)
        print(f"  {label}: {len(response)} bytes, status {status_of(response) or '-'}")
        return response

    def get(path, label):
        return request(f"GET {path} HTTP/1.1\r\nHost: bare\r\n\r\n".encode(), label)

    try:
        # Drop anything already in flight, e.g. the boot self-test's replies.
        deadline = time.time() + 0.5
        while time.time() < deadline:
            if not link.read():
                time.sleep(0.05)

        print(f"Testing {args.device}\n")

        print("1. download the seeded file")
        resp = get(args.prefix + args.seeded, f"GET {args.seeded}")
        if status_of(resp) != 200:
            failures.append(f"seeded file returned {status_of(resp)}, expected 200")
        elif not body_of(resp):
            failures.append("seeded file came back empty")
        else:
            print(f"     body: {body_of(resp)[:60]!r}")

        print("2. upload a payload")
        payload = bytes(range(32, 127)) * (args.size // 95 + 1)
        payload = payload[:args.size]
        upload = (f"PUT {args.prefix}{args.name} HTTP/1.1\r\nHost: bare\r\n"
                  f"Content-Length: {len(payload)}\r\n\r\n").encode() + payload
        resp = request(upload, f"PUT {args.name} ({len(payload)} bytes)")
        if status_of(resp) not in (200, 201):
            failures.append(f"upload returned {status_of(resp)}, expected 201")

        print("3. read it back and compare")
        resp = get(args.prefix + args.name, f"GET {args.name}")
        got = body_of(resp)
        print(f"     got {len(got)} bytes, expected {len(payload)}")
        if got != payload:
            where = next((i for i, (a, b) in enumerate(zip(got, payload)) if a != b), len(got))
            failures.append(f"round-trip mismatch: {len(got)}/{len(payload)} bytes, "
                            f"first difference at offset {where}")

        print("4. a missing file must 404")
        resp = get(args.prefix + "definitely-not-here.txt", "GET missing")
        if status_of(resp) != 404:
            failures.append(f"missing file returned {status_of(resp)}, expected 404")

        print("5. the connection must have survived all of it")
        resp = get(args.prefix + args.seeded, f"GET {args.seeded} again")
        if status_of(resp) != 200:
            failures.append("connection did not survive: keep-alive is broken")
    finally:
        link.close()

    print()
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1

    print("PASS: download, upload, round-trip and 404 all served on one connection")
    return 0


if __name__ == "__main__":
    sys.exit(main())
