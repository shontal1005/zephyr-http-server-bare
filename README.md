# Zephyr file server over a UART, without a network stack

Download files over HTTP/1.1 on **raw data buffers** — no L2 driver,
no IP, no TCP, no sockets.

The only piece of Zephyr's networking tree in the image is its bundled **HTTP
request parser** (`CONFIG_HTTP_PARSER`, the nodejs `http_parser` library).
`RawHttpServer` drives that parser directly: request packets go in through a
method, response bytes come out through a callback, and the file I/O happens in
between. What it serves is **files**: GET downloads one out of an
already-mounted directory — that is the whole surface, and any other method is
answered with 405.

## The API

One standalone class, [`include/raw_http_server.hpp`](include/raw_http_server.hpp). The output
callback is registered in the constructor; input is `net_buf` packets carrying
**arbitrary chunks of the request byte stream** — the server does the framing.
Constructing the server starts its thread — there is nothing else to start:

```cpp
NET_BUF_POOL_DEFINE(my_pool, 8, 1024, 0, NULL);

static void to_my_link(const uint8_t *data, size_t len, void *user)
{
        my_link_write(data, len);                // bytes out of the server
}

// the URL is the filesystem path: a mount at /lfs serves "GET /lfs/..."
static RawHttpServer server(to_my_link);

struct net_buf *packet = net_buf_alloc(&my_pool, K_FOREVER);
net_buf_add_mem(packet, bytes_from_my_link, n);
server.enqueue_packet(packet);           // bytes into the server
```

That is the whole surface. `enqueue_packet()` only queues — safe from any
thread or ISR — and the server's own thread does everything else: it consumes
packets in arrival order and answers each request through the output callback
as its head completes, so requests are never handled in parallel. A packet
arriving while a response is still streaming simply waits in the queue.

Because packets are arbitrary chunks, a request may be split across any number
of packets — its head is assembled in an internal buffer of
`CONFIG_RAW_HTTP_HEAD_MAX` bytes — and one packet may carry several pipelined
requests, each answered in order as its head completes. Bodies are **never**
buffered: a refused PUT's body is counted down by `Content-Length` and
discarded as it streams past, keeping the stream aligned. A zero-length packet
carries no bytes and is dropped silently.

Responses are staged in an internal buffer of `CONFIG_RAW_HTTP_FILE_CHUNK`
bytes. The bytes handed to the output callback are valid only during the
call — copy or transmit them before returning.

`RawHttpServer` knows nothing about UARTs —
[`src/uart_bridge.cpp`](src/uart_bridge.cpp) is a separate class that wires the
two together, and is easy to swap for a USB endpoint, shared memory, or a test
harness.

## Using it as a library

The repository doubles as a **Zephyr module** ([`zephyr/module.yml`](zephyr/module.yml)).
The library is `RawHttpServer` alone, in `lib/` + `include/`; everything under
`src/` — the app and the UART bridge — is sample code showing one way to feed
it. From any other application:

```cmake
# CMakeLists.txt, before find_package(Zephyr)
list(APPEND ZEPHYR_EXTRA_MODULES /path/to/zephyr-http-server-raw)
```

or list it as a project in your west manifest. Then in `prj.conf`:

```
CONFIG_RAW_HTTP_SERVER=y
```

`RAW_HTTP_SERVER` selects `CPP`, `FILE_SYSTEM`, `HTTP_PARSER`, `NET_BUF` and
`NETWORKING` (the last one exists only to reach the parser's Kconfig menu — see
the configuration notes). Everything tunable is a Kconfig option:

| Option | Default | |
|---|---|---|
| `CONFIG_RAW_HTTP_URL_MAX` | 160 | longest accepted URL (414 beyond); also bounds the filesystem path |
| `CONFIG_RAW_HTTP_HEAD_MAX` | 1024 | head-assembly buffer per instance; longest accepted request head (431 beyond) |
| `CONFIG_RAW_HTTP_FILE_CHUNK` | 1600 | response staging buffer per instance; download bytes read per output callback (min 96) |
| `CONFIG_RAW_HTTP_THREAD_STACK_SIZE` | 4096 | the server thread's stack |
| `CONFIG_RAW_HTTP_THREAD_PRIORITY` | 9 | the server thread's priority |
| `CONFIG_RAW_HTTP_LOG_LEVEL` | inf | standard per-module log level |

## Files

The URL **is** the filesystem path, resolved through whatever filesystems are
**already mounted** — the server never mounts anything itself, and Zephyr's
VFS is the validator: a URL outside every mount, a `..` climbing above a
filesystem root (littlefs refuses it) or a directory simply fails the
filesystem call and is answered with 404. The sample mounts a littlefs at
`/lfs`:

```
GET /lfs/report.bin      download the file at /lfs/report.bin
```

Downloads stream in chunks staged in the server's internal buffer —
`CONFIG_RAW_HTTP_FILE_CHUNK` bytes per output callback — so
file size is not bounded by RAM; the size comes from `fs_stat()`, so responses
carry a plain `Content-Length` — no chunked encoding, and a progress bar knows
the total up front. A missing file returns 404, a `..` in the name is rejected,
and the stream just keeps serving.

## No connection to lose

HTTP/1.1 keep-alive falls out of the design instead of being engineered in:
there is no connection object anywhere, just a reassembled byte stream.
Every request gets exactly one response — per request, not per packet: 200
with the file, 404 (nothing servable at that path), 405 (not a GET), 414
(URL longer than `CONFIG_RAW_HTTP_URL_MAX`), 431 (head outgrew
`CONFIG_RAW_HTTP_HEAD_MAX`), or 400 (malformed bytes, a chunked body, an
upgrade or CONNECT — no other protocol is spoken here). An incomplete request
earns **silence**, not an error: the head completes whenever its bytes
arrive, like on any HTTP server. Pipelining works — several requests in one
packet are answered in order. Unparseable bytes cost a **single** 400 and a
buffer reset; the stream self-heals at the next parseable request (the
refused request's own tail may earn one follow-up 400), so the client's
request/response accounting never desynchronises for long.

## How it works

Each packet's bytes go through three consumers in turn: bytes still owed to an
answered request's body are discarded first, the rest is appended to the head
assembly buffer, and every request head that completes is answered on the
spot. Parsing re-runs from the buffer start with a **freshly reinitialised
parser** on every attempt, so no parser state ever spans packets. Only two
callbacks are registered:

- `on_url` accumulates the (possibly split) URL, and fails the parse if it
  outgrows `CONFIG_RAW_HTTP_URL_MAX` rather than truncating silently
- `on_headers_complete` deliberately returns -1 to **halt the parser** at the
  end of the head — the head is everything a GET-only server needs, so the
  body is never parsed (0, 1 and 2 are magic values to this callback; -1 is a
  plain error)

That halt makes the parser's errno the entire verdict: `HPE_CB_headers_complete`
is the *success* code, `HPE_OK` honestly means the head is incomplete — wait
silently for more bytes — `HPE_CB_url` is an overlong URL (414), and anything
else is malformed bytes (400, and the buffer is dropped since boundaries are
unknown). After a complete head: an upgrade or CONNECT gets a 400 and a buffer
reset (what follows is not HTTP), a chunked body gets a 400 too (no
predeclared length to skip past), a non-GET gets its 405 as soon as the head
completes — even before its body arrives — and the body is then discarded by
`Content-Length` as it streams past; a GET has its query string cut off and
the URL goes to the VFS — a failed `fs_stat()` or `fs_open()`, or a directory,
is a 404, and a 200 streams the file back. A head that outgrows a full
assembly buffer is answered 431 and the buffer reset.

There are no sockets and no state machine beyond the parser's own. The class
owns one thread and one packet queue — and **no mutex**: the
server thread is the only thing touching request state, `enqueue_packet()`
only touches the queue (a `k_fifo`, safe from threads and ISRs), and the
single-consumer queue is what serialises requests.

### Threads

- the `RawHttpServer` thread: takes packets off the queue and runs the parser,
  the file I/O and the output callback, one request at a time
- a feed thread owned by `UartBridge`: it wraps whatever the RX ring holds in
  packets as bytes arrive — no framing, the server reassembles — and exists
  because allocating a packet may block on an exhausted pool, which the UART
  ISR may not do

## Building and running

With this repo as a west manifest (pulls Zephyr v4.4.0 plus littlefs, nothing
else):

```sh
mkdir bare-http && cd bare-http
git clone <this repo> zephyr-http-server-raw
west init -l zephyr-http-server-raw
west update
west build -b native_sim zephyr-http-server-raw
./build/zephyr/zephyr.exe
```

In an existing workspace, just `west build -b native_sim` the project
directory.

`native_sim` has nothing mounted, so the sample mounts a littlefs on the flash
simulator at `/lfs` and seeds two files into it: `hello.txt` and `big.bin`, a
5035-byte position-dependent pattern that exceeds `CONFIG_RAW_HTTP_FILE_CHUNK`
and so downloads over several chunks (the test suite regenerates it byte for
byte). The board overlay grows `storage_partition` from its stock 16 KiB to
1 MiB — littlefs metadata eats most of 16 KiB, and seeding the demo files
would otherwise fail with `-ENOSPC`. A real board would have mounted its
storage during boot and simply passed the path to the constructor.

At boot the sample injects five requests — a 200, a 404, a 405, a 400 and a
split 200 — so the mechanism is visible without a terminal attached:

```
<inf> uart_bridge: UART uart_1 wired to the HTTP server
<inf> http_server_bare: --> request 1: 43 bytes in one packet
<inf> http_server_bare: --> request 2: 45 bytes in one packet
<inf> http_server_bare: --> request 3: 66 bytes in one packet
<inf> http_server_bare: --> request 4: 20 bytes in one packet
<inf> http_server_bare: --> request 5: 43 bytes in two packets
<inf> http_server_bare: Self-test done, now serving the UART forever
<inf> raw_http: GET /lfs/hello.txt
<inf> raw_http: GET /lfs/missing.txt -> -2
<wrn> raw_http: PUT refused: GET is the whole surface
<wrn> raw_http: Unparseable bytes dropped (HPE_INVALID_METHOD)
<inf> raw_http: GET /lfs/hello.txt
```

The five demonstrate a download of the seeded file (200), a missing file
(404), a PUT whose body is skipped (405), bytes that are not HTTP (a single
400, then the stream heals), and a GET deliberately split across two packets
and reassembled (200). Set `CONFIG_APP_SELFTEST=n` to skip the injected
requests.

The UART is selected by the `http-uart` devicetree alias; the supplied
`native_sim` overlay points it at `uart1` so the console keeps `uart0`.
`native_sim` prints the pseudoterminal backing `uart1` on startup — attach to
it in **raw mode** (the default line discipline would mangle an HTTP byte
stream):

```
uart_1 connected to pseudotty: /dev/pts/9
```

## Testing against a real board

[`tests/http_over_serial_test.py`](tests/http_over_serial_test.py) drives the
whole thing over a serial device given as an argument:

```sh
./tests/http_over_serial_test.py /dev/ttyUSB0
./tests/http_over_serial_test.py /dev/ttyACM0 --baud 921600
```

It downloads the seeded `hello.txt`, downloads the multi-chunk `big.bin` and
compares it byte for byte against the regenerated pattern, checks that a PUT
is refused with 405 and that a missing file 404s, sends a request split into
two writes to prove it is reassembled, and finally re-downloads to prove the
stream survived all of it. Exit status is 0 on pass, 1 on failure.
`native_sim` works through the same path — pass the pseudoterminal it prints
instead of a real device.

pyserial is used when present and is required for `--baud`; without it the
device is opened raw, which is all a pty needs.

## Python client

[`client/raw_http_client.py`](client/raw_http_client.py) is the host-side
mirror of the server: `RawHttpClient` speaks the HTTP but knows nothing about
the link — subclasses plug in the transport by overriding `send()` and
`receive()`, just as the server delegates its bytes to a callback.
`SerialRawHttpClient` is the UART implementation (pyserial when present, a raw
fd otherwise — all a pty needs):

```python
from raw_http_client import SerialRawHttpClient

with SerialRawHttpClient("/dev/pts/9") as client:
    client.drain()                              # boot self-test replies
    client.download("hello.txt", "hello.txt")
    status, body = client.request(b"PUT /lfs/x HTTP/1.1\r\n\r\n")  # 405
```

`download()` is the surface meant for real use; `request()` sends one raw
request and returns `(status, body)` without judging the status — it is how
the tests assert the refusals (405, 414, 431, 400). Responses are read from a
persistent buffer, so pipelined responses arriving in one `receive()` are not
lost (`drain()` clears it). The file doubles as a tool:
`./client/raw_http_client.py /dev/ttyUSB0 download report.bin report.bin`.

A pytest suite drives the client end to end — against an auto-launched
`native_sim` by default, or against a real board over plain UART. It
deliberately abuses the transport framing: requests split at arbitrary and
every interesting boundary, dribbled a byte at a time, pipelined several to a
write, refused bodies arriving late — all must come back correct and in
order:

```sh
pytest tests/                                  # launches build/zephyr/zephyr.exe
pytest tests/ --device /dev/ttyUSB0 --baud 115200   # a real board
```

## Configuration notes

`RAW_HTTP_SERVER` **selects** `NETWORKING` for one reason only: the parser
library's Kconfig lives under the networking menu. `select` forces just that
one symbol — sub-options like `NET_NATIVE` follow their own defaults, so the
module's Kconfig flips `NET_NATIVE` to `default n` (module Kconfig files are
sourced before the subsystem tree, and the first satisfied default wins).
The result: no IP stack, no sockets, no TCP in the image, and an application
that also wants real networking just sets `CONFIG_NET_NATIVE=y` back.

One caveat: **board** `Kconfig.defconfig` files are sourced before modules and
deliberately outrank them. `native_sim` switches Ethernet on whenever
networking is in the build, so the sample's `prj.conf` carries an explicit
`CONFIG_NET_L2_ETHERNET=n` — expect the same on any board that self-enables
its network driver.

## Things to watch out for

- The output callback runs on the **server's thread**, and the bytes it
  receives are valid only during the call: copy or transmit them before
  returning. Blocking there is fine — it is the natural flow control against a
  slow link.
- Never assume a fixed callback size: one response may arrive over several
  calls, and `len` is whatever the server produced in one go.
- `Expect: 100-continue` is not implemented — but a non-GET is answered as
  soon as its head completes, so a client pausing for the 100 gets its 405
  instead, and its body is discarded when (and if) it arrives.
- A request that carries `Connection: close` is served, but there is no
  connection to close — the stream simply keeps serving.
- `UartBridge` transmits with `uart_poll_out()`, which busy-waits per byte.
  Fine at sane baud rates; switch to interrupt-driven TX if you push large
  responses at high speed.
- If the UART RX ring (`UART_BRIDGE_RING_SIZE`) overflows, bytes are dropped
  and the bridge logs the exact count. Nothing else is needed: the mangled
  request answers for itself with a 400, and the stream self-heals at the
  next request boundary. At any real baud rate the feed thread drains far
  faster than bytes arrive.
- On `native_sim` that overflow is easy to trip artificially: a pseudoterminal
  has no baud rate, and native_pty's interrupt-emulation thread runs at
  `K_HIGHEST_THREAD_PRIO` without sleeping. The client paces its writes by
  default, and `--pace 0` turns it off.
- HTTP has no framing below it, and there is no TCP providing reliability or
  ordering. Bytes must arrive in order, exactly once; a dropped or duplicated
  byte mangles its request, which costs a single 400 and a reset — the stream
  heals at the next parseable request. Over a noisy UART, add framing or use
  a clean link.

## License

Apache-2.0, matching Zephyr. `README.rst` is the upstream-style version of this
document, kept for a possible submission to the Zephyr tree.
