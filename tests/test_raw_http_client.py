# Copyright (c) 2026 Zephyr Project contributors
#
# SPDX-License-Identifier: Apache-2.0
"""
End-to-end tests: the Python client against the firmware, over plain UART.

Runs against native_sim by default; pass ``--device /dev/ttyUSB0`` (and
``--baud``) to run the very same suite against a real board.

The server is GET-only and reassembles the request byte stream itself, so
these tests deliberately abuse the transport framing: requests split at
arbitrary points, dribbled a byte at a time, pipelined several to a write.
The refusal tests (405, 414, 431, 400) send raw requests via
``client.request()`` and look at the status; most of them finish with a
successful download to prove the stream healed.
"""

import time

import pytest

from raw_http_client import RawHttpError  # noqa: F401 - conftest adds client/

SEEDED_NAME = "hello.txt"
SEEDED_BODY = b"hello from the bare HTTP server\n"

# Seeded at boot precisely because it cannot be uploaded: its size exceeds
# CONFIG_RAW_HTTP_FILE_CHUNK, so downloading it takes several chunks.
BIG_NAME = "big.bin"
BIG_SIZE = 5035

GET_SEEDED = f"GET /lfs/{SEEDED_NAME} HTTP/1.1\r\nHost: raw\r\n\r\n".encode()


def payload_of(size):
    """Deterministic position-dependent bytes, so a mismatch shows its offset.

    Mirrors the firmware's seeding loop in main.cpp byte for byte.
    """
    return bytes((i * 7 + i // 251) & 0xFF for i in range(size))


def assert_quiet(client, settle=0.3):
    """Assert the server sends nothing for `settle` seconds."""
    deadline = time.time() + settle
    while time.time() < deadline:
        assert client.receive() == b"", "server responded when it should wait"
        time.sleep(0.02)


def assert_downloads_seeded(client, tmp_path, name="after"):
    """The canonical stream-health probe: the seeded file still downloads."""
    dest = tmp_path / name
    assert client.download(SEEDED_NAME, dest) == len(SEEDED_BODY)
    assert dest.read_bytes() == SEEDED_BODY


# --------------------------------------------------------------------------
# Downloads
# --------------------------------------------------------------------------


def test_download_seeded_file(client, tmp_path):
    assert_downloads_seeded(client, tmp_path)


def test_download_spans_several_chunks(client, tmp_path):
    dest = tmp_path / "big.bin"

    n = client.download(BIG_NAME, dest)

    assert n == BIG_SIZE
    assert dest.read_bytes() == payload_of(BIG_SIZE)


def test_query_string_is_stripped(client):
    request = (f"GET /lfs/{SEEDED_NAME}?some=query&goes=here "
               f"HTTP/1.1\r\nHost: raw\r\n\r\n").encode()

    status, body = client.request(request)

    assert status == 200
    assert body == SEEDED_BODY


def test_missing_file_raises_file_not_found(client, tmp_path):
    with pytest.raises(FileNotFoundError):
        client.download("definitely-not-here.bin", tmp_path / "x")


def test_directory_is_404(client):
    status, _ = client.request(b"GET /lfs HTTP/1.1\r\nHost: raw\r\n\r\n")

    assert status == 404


def test_path_outside_any_mount_is_404(client):
    status, _ = client.request(b"GET /nowhere/x HTTP/1.1\r\nHost: raw\r\n\r\n")

    assert status == 404


# --------------------------------------------------------------------------
# Split, dribbled and pipelined requests: the server does the framing
# --------------------------------------------------------------------------


def test_split_request_is_reassembled(client, tmp_path):
    """Half a request earns silence, not an error - the head completes
    whenever its bytes arrive."""
    half = len(GET_SEEDED) // 2

    client.send(GET_SEEDED[:half])
    assert_quiet(client)
    client.send(GET_SEEDED[half:])

    status, body = client._read_response()

    assert status == 200
    assert body == SEEDED_BODY
    assert_downloads_seeded(client, tmp_path)


def test_request_dribbled_a_byte_at_a_time(client):
    """The pathological split: every byte is its own write."""
    for i in range(len(GET_SEEDED)):
        client.send(GET_SEEDED[i:i + 1])
        time.sleep(0.002)

    status, body = client._read_response()

    assert status == 200
    assert body == SEEDED_BODY


def test_split_at_every_interesting_boundary(client):
    """Split exactly at the request-line end, mid-header and inside the
    final CRLFCRLF - all must reassemble."""
    for cut in (GET_SEEDED.index(b"\r\n") + 2,      # after the request line
                GET_SEEDED.index(b"Host") + 2,      # mid-header-name
                len(GET_SEEDED) - 2):               # inside the final CRLFCRLF
        client.send(GET_SEEDED[:cut])
        time.sleep(0.05)
        client.send(GET_SEEDED[cut:])

        status, body = client._read_response()

        assert status == 200, f"split at {cut} failed"
        assert body == SEEDED_BODY


def test_pipelined_requests_in_one_write(client):
    """Two requests in one write get two responses, in order."""
    missing = b"GET /lfs/nope.bin HTTP/1.1\r\nHost: raw\r\n\r\n"

    client.send(GET_SEEDED + missing)

    status1, body1 = client._read_response()
    status2, _ = client._read_response()

    assert (status1, status2) == (200, 404)
    assert body1 == SEEDED_BODY


def test_pipelined_batch_split_mid_request(client):
    """A full request plus the start of the next in one write: the first
    is answered at once, the second when its remainder arrives."""
    half = len(GET_SEEDED) // 2

    client.send(GET_SEEDED + GET_SEEDED[:half])

    status1, body1 = client._read_response()
    assert status1 == 200
    assert body1 == SEEDED_BODY

    client.send(GET_SEEDED[half:])

    status2, body2 = client._read_response()
    assert status2 == 200
    assert body2 == SEEDED_BODY


# --------------------------------------------------------------------------
# Refused methods: 405, body skipped by Content-Length
# --------------------------------------------------------------------------


def test_put_is_refused_with_405_and_body_skipped(client, tmp_path):
    request = (b"PUT /lfs/pytest_upload.bin HTTP/1.1\r\nHost: raw\r\n"
               b"Content-Length: 4\r\n\r\nnope")

    status, _ = client.request(request)

    assert status == 405
    # The body must have been skipped exactly, or this download would
    # be parsed against misaligned bytes.
    assert_downloads_seeded(client, tmp_path)


def test_put_body_arriving_after_the_head_is_skipped(client, tmp_path):
    """The 405 goes out as soon as the head is complete; body bytes that
    trickle in afterwards are discarded, not parsed."""
    head = (b"PUT /lfs/pytest_upload.bin HTTP/1.1\r\nHost: raw\r\n"
            b"Content-Length: 10\r\n\r\n")

    client.send(head)
    status, _ = client._read_response()
    assert status == 405

    client.send(b"0123456789")  # the late body, silently discarded

    assert_downloads_seeded(client, tmp_path)


@pytest.mark.parametrize("request_bytes", [
    b"POST /lfs/x HTTP/1.1\r\nHost: raw\r\nContent-Length: 5\r\n\r\nhello",
    b"DELETE /lfs/hello.txt HTTP/1.1\r\nHost: raw\r\n\r\n",
    b"HEAD /lfs/hello.txt HTTP/1.1\r\nHost: raw\r\n\r\n",
    b"OPTIONS /lfs/hello.txt HTTP/1.1\r\nHost: raw\r\n\r\n",
], ids=["POST", "DELETE", "HEAD", "OPTIONS"])
def test_non_get_methods_are_refused_with_405(client, tmp_path, request_bytes):
    status, _ = client.request(request_bytes)

    assert status == 405
    assert_downloads_seeded(client, tmp_path)


# --------------------------------------------------------------------------
# Upgrades, oversize and garbage: 400, 414, 431 - and the stream heals
# --------------------------------------------------------------------------


def test_connect_is_refused_with_400(client, tmp_path):
    status, _ = client.request(
        b"CONNECT example.com:80 HTTP/1.1\r\nHost: raw\r\n\r\n")

    assert status == 400
    assert_downloads_seeded(client, tmp_path)


def test_upgrade_is_refused_with_400(client, tmp_path):
    """An otherwise perfectly valid GET: the upgrade is what is refused."""
    request = (f"GET /lfs/{SEEDED_NAME} HTTP/1.1\r\nHost: raw\r\n"
               f"Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n").encode()

    status, _ = client.request(request)

    assert status == 400
    assert_downloads_seeded(client, tmp_path)


def test_overlong_url_is_refused_with_414(client, tmp_path):
    name = "x" * 300  # comfortably past CONFIG_RAW_HTTP_URL_MAX (160)
    request = f"GET /lfs/{name} HTTP/1.1\r\nHost: raw\r\n\r\n".encode()

    status, _ = client.request(request)

    assert status == 414
    # The refusal resets the stream mid-request, so the request's own
    # tail may earn one follow-up 400: drop it before probing health.
    client.drain(settle=0.3)
    assert_downloads_seeded(client, tmp_path)


def test_oversized_head_is_refused_with_431(client, tmp_path):
    """A head that outgrows CONFIG_RAW_HTTP_HEAD_MAX (1024) but keeps every
    line legal - only its size is wrong."""
    padding = "".join(f"X-Pad-{i}: {'a' * 300}\r\n" for i in range(4))
    request = (f"GET /lfs/{SEEDED_NAME} HTTP/1.1\r\nHost: raw\r\n"
               f"{padding}\r\n").encode()
    assert len(request) > 1024

    status, _ = client.request(request)

    assert status == 431
    client.drain(settle=0.3)
    assert_downloads_seeded(client, tmp_path)


def test_malformed_bytes_get_a_400(client, tmp_path):
    status, _ = client.request(b"this is not HTTP\r\n\r\n")

    assert status == 400
    assert_downloads_seeded(client, tmp_path)


def test_binary_garbage_gets_a_400(client, tmp_path):
    status, _ = client.request(bytes(range(1, 32)) * 3)

    assert status == 400
    client.drain(settle=0.3)
    assert_downloads_seeded(client, tmp_path)


# --------------------------------------------------------------------------
# Longevity
# --------------------------------------------------------------------------


def test_stream_survives_many_requests(client, tmp_path):
    """A refusal in the middle must not affect the requests after it."""
    for i in range(2):
        assert client.download(SEEDED_NAME, tmp_path / f"a{i}") == len(SEEDED_BODY)

    with pytest.raises(FileNotFoundError):
        client.download("nope.bin", tmp_path / "x")

    assert client.download(SEEDED_NAME, tmp_path / "after") == len(SEEDED_BODY)


def test_mixed_traffic_marathon(client, tmp_path):
    """Downloads, refusals, splits and garbage interleaved, twice over -
    every response must still arrive, correct and in order."""
    for round_no in range(2):
        assert_downloads_seeded(client, tmp_path, f"m{round_no}a")

        status, _ = client.request(
            b"PUT /lfs/m.bin HTTP/1.1\r\nHost: raw\r\n"
            b"Content-Length: 6\r\n\r\nabcdef")
        assert status == 405

        half = len(GET_SEEDED) // 2
        client.send(GET_SEEDED[:half])
        time.sleep(0.03)
        client.send(GET_SEEDED[half:])
        status, body = client._read_response()
        assert (status, body) == (200, SEEDED_BODY)

        status, _ = client.request(b"junk\r\n\r\n")
        assert status == 400

        assert_downloads_seeded(client, tmp_path, f"m{round_no}b")
