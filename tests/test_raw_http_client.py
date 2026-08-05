# Copyright (c) 2026 Zephyr Project contributors
#
# SPDX-License-Identifier: Apache-2.0
"""
End-to-end tests: the Python client against the firmware, over plain UART.

Runs against native_sim by default; pass ``--device /dev/ttyUSB0`` (and
``--baud``) to run the very same suite against a real board.
"""

import pytest

from raw_http_client import RawHttpError  # noqa: F401 - conftest adds client/

SEEDED_NAME = "hello.txt"
SEEDED_BODY = b"hello from the bare HTTP server\n"


def payload_of(size):
    """Deterministic position-dependent bytes, so a mismatch shows its offset."""
    return bytes((i * 7 + i // 251) & 0xFF for i in range(size))


def test_download_seeded_file(client, tmp_path):
    dest = tmp_path / "hello.txt"

    n = client.download(SEEDED_NAME, dest)

    assert n == len(SEEDED_BODY)
    assert dest.read_bytes() == SEEDED_BODY


@pytest.mark.parametrize("size", [0, 1, 100, 5035, 20000])
def test_upload_download_round_trip(client, tmp_path, size):
    src = tmp_path / "src.bin"
    back = tmp_path / "back.bin"
    src.write_bytes(payload_of(size))
    name = f"pytest_{size}.bin"

    client.upload(src, name)
    n = client.download(name, back)

    assert n == size
    assert back.read_bytes() == payload_of(size)


def test_missing_file_raises_file_not_found(client, tmp_path):
    with pytest.raises(FileNotFoundError):
        client.download("definitely-not-here.bin", tmp_path / "x")


def test_shorter_reupload_truncates(client, tmp_path):
    """FS_O_TRUNC: the tail of an earlier, longer upload must not survive."""
    long_src = tmp_path / "long.bin"
    short_src = tmp_path / "short.bin"
    back = tmp_path / "back.bin"
    long_src.write_bytes(payload_of(2000))
    short_src.write_bytes(b"\xAA" * 100)

    client.upload(long_src, "pytest_trunc.bin")
    client.upload(short_src, "pytest_trunc.bin")
    n = client.download("pytest_trunc.bin", back)

    assert n == 100
    assert back.read_bytes() == b"\xAA" * 100


def test_stream_survives_many_requests(client, tmp_path):
    """Keep-alive, with a 404 in the middle that must not kill the stream."""
    for i in range(2):
        assert client.download(SEEDED_NAME, tmp_path / f"a{i}") == len(SEEDED_BODY)

    with pytest.raises(FileNotFoundError):
        client.download("nope.bin", tmp_path / "x")

    assert client.download(SEEDED_NAME, tmp_path / "after") == len(SEEDED_BODY)
