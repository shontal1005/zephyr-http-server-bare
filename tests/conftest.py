# Copyright (c) 2026 Zephyr Project contributors
#
# SPDX-License-Identifier: Apache-2.0
"""
Fixtures for the client test suite.

With no options the suite launches native_sim (build/zephyr/zephyr.exe) and
talks to the pseudoterminal it prints. Point it at real hardware instead::

    pytest tests/ --device /dev/ttyUSB0 --baud 115200
"""

import os
import re
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), os.pardir, "client"))

from raw_http_client import SerialRawHttpClient  # noqa: E402

PTY_RE = re.compile(r"uart_1 connected to pseudotty: (/dev/pts/\d+)")


def pytest_addoption(parser):
    parser.addoption("--device", default=None,
                     help="serial device of a real board; default: launch native_sim")
    parser.addoption("--baud", type=int, default=115200,
                     help="baud rate for --device (needs pyserial)")


@pytest.fixture(scope="session")
def device(request, tmp_path_factory):
    """The serial device under test: a real one, or a freshly launched sim."""
    dev = request.config.getoption("--device")
    if dev:
        yield dev
        return

    exe = os.path.join(os.path.dirname(__file__), os.pardir,
                       "build", "zephyr", "zephyr.exe")
    if not os.path.exists(exe):
        pytest.skip("no --device given and build/zephyr/zephyr.exe is missing")

    # zephyr.exe drops a flash.bin into its cwd; keep that out of the repo.
    workdir = tmp_path_factory.mktemp("sim")
    log_path = workdir / "boot.log"

    with open(log_path, "wb") as log:
        proc = subprocess.Popen([os.path.abspath(exe)], cwd=workdir,
                                stdout=log, stderr=subprocess.STDOUT)

    try:
        deadline = time.time() + 10
        pty = None
        while pty is None and time.time() < deadline:
            match = PTY_RE.search(log_path.read_text(errors="replace"))
            if match:
                pty = match.group(1)
            else:
                time.sleep(0.1)

        if pty is None:
            pytest.fail("native_sim did not print its pty within 10 s")

        yield pty
    finally:
        proc.kill()
        proc.wait()


@pytest.fixture()
def client(request, device):
    """A drained SerialRawHttpClient on the device, fresh for every test."""
    with SerialRawHttpClient(device, baud=request.config.getoption("--baud"),
                             timeout=10.0) as c:
        c.drain()
        yield c
