#!/usr/bin/env python3
#
# Copyright (c) 2026 Zephyr Project contributors
#
# SPDX-License-Identifier: Apache-2.0
"""
A client for the bare HTTP file server, transport-agnostic like the server.

`RawHttpClient` mirrors `RawHttpServer`: it knows HTTP but nothing about the
link underneath. Subclasses supply the byte pipe by overriding two methods -
send() and receive() - exactly as the C++ class delegates its bytes to an
output callback and an input() caller. `SerialRawHttpClient` is the UART
implementation used by this project.

The server serves GET only - anything else is answered with 405 - and
reassembles the request byte stream itself, so requests may be split or
batched arbitrarily on the wire.

The file is also a tool::

    ./client/raw_http_client.py /dev/pts/9 download hello.txt /tmp/hello.txt
"""

import argparse
import os
import sys
import time

DEFAULT_TIMEOUT = 5.0


class RawHttpError(Exception):
    """An HTTP response with an unexpected status."""

    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


class RawHttpClient:
    """Download files over HTTP/1.1 on a raw byte stream.

    The "connection" is whatever byte pipe the subclass owns, so it never
    opens or closes: every request rides the same stream and responses come
    back in order. Responses always carry Content-Length - the server never
    chunks - so one response is the head up to CRLFCRLF plus exactly that
    many body bytes.
    """

    def __init__(self, timeout=DEFAULT_TIMEOUT, prefix="/lfs/",
                 pace=256, gap=0.005):
        """
        :param timeout: seconds to wait for one complete response
        :param prefix: URL prefix files are served under - the server maps
                       URLs straight to filesystem paths, so this is the
                       board's mount point (the sample mounts at /lfs)
        :param pace: bytes per send() call, 0 to write everything in one go
        :param gap: seconds between paced sends
        """
        self.timeout = timeout
        self.prefix = prefix
        self.pace = pace
        self.gap = gap
        # One receive() may span two responses (pipelining): whatever
        # follows the response being read stays here for the next read.
        self._rx = bytearray()

    def send(self, data):
        """Write bytes to the link. Override in a subclass."""
        raise NotImplementedError

    def receive(self):
        """Return whatever bytes are available right now, possibly b''.

        Override in a subclass. Must not block.
        """
        raise NotImplementedError

    def drain(self, settle=0.5):
        """Discard stale bytes until the link is quiet for `settle` seconds.

        At boot the firmware's self-test pushes five responses out the same
        link; call this once before the first request.
        """
        self._rx.clear()
        deadline = time.time() + settle
        while time.time() < deadline:
            if self.receive():
                deadline = time.time() + settle  # still talking; start over
            else:
                time.sleep(0.05)

    def download(self, remote_name, dest_path):
        """GET a file and write its body to `dest_path`.

        :return: number of body bytes written
        :raises FileNotFoundError: on a 404
        :raises RawHttpError: on any other non-200 status
        """
        request = (f"GET {self.prefix}{remote_name} HTTP/1.1\r\n"
                   f"Host: raw\r\n\r\n").encode()
        status, body = self._round_trip(request)

        if status == 404:
            raise FileNotFoundError(f"{self.prefix}{remote_name}: 404 Not Found")
        if status != 200:
            raise RawHttpError(status, f"GET {remote_name} returned {status}")

        with open(dest_path, "wb") as f:
            f.write(body)
        return len(body)

    def request(self, raw):
        """Send one raw request and return ``(status, body)``.

        No status is an error here - this is how tests assert the
        refusals (405 for a non-GET, 414 for an overlong URL, 431 for
        an oversized head, 400 for malformed bytes). download() is the
        surface meant for real use.
        """
        return self._round_trip(raw)

    def _round_trip(self, request):
        """Send one request paced, read one response, return (status, body)."""
        self._send_paced(request)
        return self._read_response()

    def _send_paced(self, data):
        """send() in slices so a fast host cannot outrun the board's RX ring.

        A real UART paces itself at the baud rate; a pseudoterminal has no
        rate limit at all, so unpaced bursts overflow the firmware's RX ring.
        """
        if self.pace <= 0:
            self.send(data)
            return

        for i in range(0, len(data), self.pace):
            self.send(data[i:i + self.pace])
            time.sleep(self.gap)

    def _read_response(self):
        """Read one response: head up to CRLFCRLF, then Content-Length bytes.

        Consumes exactly one response from the persistent buffer; bytes
        beyond it (a pipelined next response) stay for the next call.
        """
        buf = self._rx
        deadline = time.time() + self.timeout
        head = None
        length = 0

        while time.time() < deadline:
            if head is None:
                split, sep, _ = bytes(buf).partition(b"\r\n\r\n")
                if sep:
                    head = split
                    del buf[:len(head) + 4]
                    for line in head.lower().split(b"\r\n"):
                        if line.startswith(b"content-length:"):
                            length = int(line.split(b":", 1)[1])
                            break

            if head is not None and len(buf) >= length:
                body = bytes(buf[:length])
                del buf[:length]
                return self._status_of(head), body

            chunk = self.receive()
            if chunk:
                buf.extend(chunk)
            else:
                time.sleep(0.01)

        raise TimeoutError(f"no complete response within {self.timeout} s")

    @staticmethod
    def _status_of(head):
        parts = head.split(b"\r\n", 1)[0].split()
        if len(parts) >= 2 and parts[1].isdigit():
            return int(parts[1])
        return 0


class SerialRawHttpClient(RawHttpClient):
    """RawHttpClient over a serial device.

    pyserial is used when available (needed to set a real baud rate); without
    it the device is opened as a raw non-blocking fd, which is all a
    native_sim pseudoterminal needs - `baud` is then ignored.
    """

    def __init__(self, device, baud=115200, **kwargs):
        super().__init__(**kwargs)
        self._serial = None
        self._fd = None

        try:
            import serial  # noqa: PLC0415
        except ImportError:
            serial = None

        if serial is not None:
            self._serial = serial.Serial(device, baud, timeout=0)
            return

        import termios  # noqa: PLC0415
        import tty  # noqa: PLC0415

        self._fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            # A pty slave keeps the default line discipline, which would
            # mangle an HTTP byte stream (ICRNL, OPOST, ECHO). Raw mode is
            # mandatory.
            tty.setraw(self._fd)
        except termios.error:
            pass  # not a tty; a plain pipe or file is fine as-is

    def send(self, data):
        """Write bytes to the serial device."""
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

    def receive(self):
        """Whatever is available right now, possibly b''."""
        if self._serial is not None:
            return self._serial.read(4096)
        try:
            return os.read(self._fd, 4096)
        except (BlockingIOError, OSError):
            return b""

    def close(self):
        """Close the underlying device. Safe to call more than once."""
        if self._serial is not None:
            self._serial.close()
            self._serial = None
        elif self._fd is not None:
            os.close(self._fd)
            self._fd = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("device", help="serial device the board's http-uart is on")
    parser.add_argument("--baud", type=int, default=115200,
                        help="baud rate (needs pyserial; ignored for a pty)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help="seconds to wait for one response")
    parser.add_argument("--prefix", default="/lfs/",
                        help="URL prefix files are served under "
                             "(the board's mount point)")
    parser.add_argument("--pace", type=int, default=256,
                        help="bytes per write, 0 to disable pacing")
    parser.add_argument("--gap", type=float, default=0.005,
                        help="seconds between paced writes")

    sub = parser.add_subparsers(dest="command", required=True)
    download = sub.add_parser("download", help="GET a remote file")
    download.add_argument("remote", help="remote file name")
    download.add_argument("dest", help="local path to write")

    args = parser.parse_args()

    with SerialRawHttpClient(args.device, baud=args.baud, timeout=args.timeout,
                             prefix=args.prefix, pace=args.pace,
                             gap=args.gap) as client:
        client.drain()  # boot self-test responses may still be in flight
        try:
            n = client.download(args.remote, args.dest)
            print(f"{args.remote} -> {args.dest}: {n} bytes")
        except (FileNotFoundError, RawHttpError, TimeoutError) as err:
            sys.exit(f"error: {err}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
