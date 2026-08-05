# Zephyr file server over a UART, without a network stack

Download and upload files over HTTP/1.1 on **raw data buffers** — no L2 driver,
no IP, no TCP, no sockets.

The only piece of Zephyr's networking tree in the image is its bundled **HTTP
request parser** (`CONFIG_HTTP_PARSER`, the nodejs `http_parser` library).
`RawHttpServer` drives that parser directly: request bytes go in through a
method, response bytes come out through a callback, and the parser callbacks do
the file I/O in between. What it serves is **files**: GET downloads one out of
an already-mounted directory, PUT and POST upload one into it.

## The API

One standalone class, [`include/raw_http_server.hpp`](include/raw_http_server.hpp). The output
callback is registered in the constructor; input is a buffer and a size. There
is nothing to start:

```cpp
static void to_my_link(const uint8_t *data, size_t len, void *user)
{
        my_link_write(data, len);        // bytes out of the server
}

// second argument is the already-mounted directory files live in
static RawHttpServer server(to_my_link, "/lfs");

server.input(bytes_from_my_link, n);     // bytes into the server
```

That is the whole surface. Everything happens synchronously inside `input()`:
by the time it returns, the response has been handed to the output callback.
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

`RAW_HTTP_SERVER` selects `CPP`, `FILE_SYSTEM`, `HTTP_PARSER` and `NETWORKING`
(the last one exists only to reach the parser's Kconfig menu — see the
configuration notes). Everything tunable is a Kconfig option:

| Option | Default | |
|---|---|---|
| `CONFIG_RAW_HTTP_FILES_PREFIX` | `/files/` | URL prefix files are served under |
| `CONFIG_RAW_HTTP_URL_MAX` | 160 | longest accepted URL (414 beyond) |
| `CONFIG_RAW_HTTP_FILE_CHUNK` | 1600 | download buffer, bytes per instance |
| `CONFIG_RAW_HTTP_LOG_LEVEL` | inf | standard per-module log level |

## Files

The constructor takes the path of a directory that is **already mounted** — the
server never mounts anything itself. Files are served under `/files/`:

```
GET  /files/report.bin      download <fs_root>/report.bin
PUT  /files/report.bin      upload the request body to that path
POST /files/report.bin      same as PUT
```

Downloads stream through a `CONFIG_RAW_HTTP_FILE_CHUNK` buffer (1600 bytes by default), so file
size is not bounded by RAM; the size comes from `fs_stat()`, so responses carry
a plain `Content-Length` — no chunked encoding, and a progress bar knows the
total up front. Uploads never touch that buffer: body fragments are written to
the file straight out of the caller's input buffer as the parser delivers them,
so they are not bounded either. A missing file returns 404, a `..` in the name
is rejected, and the stream just keeps serving.

## No connection to lose

HTTP/1.1 keep-alive falls out of the design instead of being engineered in:
there is no connection object anywhere, just a resumable parser on a byte
stream. Requests may be split across any number of `input()` calls, and one
call may carry several requests. A malformed request is answered with a 400 and
the parser resets — nothing is lost beyond the malformed bytes themselves.

## How it works

`http_parser_execute()` is a resumable, byte-at-a-time parser: feed it whatever
arrived and it fires callbacks at message boundaries. Four of them do all the
work:

- `on_url` accumulates the (possibly split) URL
- `on_headers_complete` opens the destination file for PUT/POST
- `on_body` appends each body fragment to that file
- `on_message_complete` answers: streams the file back for GET, closes and
  201s for uploads, or emits the recorded error status

There are no sockets, no threads and no state machine beyond the parser's own.
The class owns one mutex (so several threads may call `input()`), one file
handle and one download buffer.

### Threads

- a feed thread owned by `UartBridge`, because the UART ISR may not block and
  `input()` does file I/O
- that's it — `RawHttpServer` itself creates none

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
simulator at `/lfs` and seeds a `hello.txt` into it. The board overlay grows
`storage_partition` from its stock 16 KiB to 1 MiB — littlefs metadata eats most
of 16 KiB, and uploads otherwise fail with `-ENOSPC` after a few hundred bytes.
A real board would have mounted its storage during boot and simply passed the
path to the constructor.

At boot the sample injects four requests — a download, an upload, a read-back
and a 404 — so the mechanism is visible without a terminal attached:

```
<inf> uart_bridge: UART uart_1 wired to the HTTP server
<inf> http_server_bare: --> request 1: feeding 46 bytes in two chunks
<inf> raw_http: GET /lfs/hello.txt
<inf> http_server_bare: --> request 2: feeding 87 bytes in two chunks
<inf> raw_http: PUT /lfs/upload.txt
<inf> http_server_bare: --> request 3: feeding 47 bytes in two chunks
<inf> raw_http: GET /lfs/upload.txt
<inf> http_server_bare: --> request 4: feeding 48 bytes in two chunks
<inf> raw_http: GET /files/missing.txt -> -2
<inf> http_server_bare: Self-test done, now serving the UART forever
```

Each request is handed over in **two separate calls** on purpose: the parser
consumes a byte stream, so requests may be split across as many buffers as the
transport produces. Set `CONFIG_APP_SELFTEST=n` to skip the injected requests.

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

It downloads the seeded file, uploads a generated payload, reads it back and
compares byte for byte, checks that a missing file 404s, and finally
re-downloads to prove the stream survived all of it. Exit status is 0 on pass,
1 on failure. `native_sim` works through the same path — pass the
pseudoterminal it prints instead of a real device.

pyserial is used when present and is required for `--baud`; without it the
device is opened raw, which is all a pty needs.

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

- The output callback runs **inside `input()`**, on the caller's thread. Never
  call `input()` from it.
- Never assume a fixed callback size: one response may arrive over several
  calls, and `len` is whatever the server produced in one go.
- `Expect: 100-continue` is not implemented. Clients that send it (curl does
  for large uploads) will pause briefly before sending the body; pass
  `-H 'Expect:'` to avoid the delay.
- A request that carries `Connection: close` is served, but there is no
  connection to close — the stream simply keeps serving.
- `UartBridge` transmits with `uart_poll_out()`, which busy-waits per byte.
  Fine at sane baud rates; switch to interrupt-driven TX if you push large
  responses at high speed.
- If the UART RX ring (`UART_BRIDGE_RING_SIZE`) overflows, bytes are dropped
  and the request stream desynchronises. The bridge logs an error. At any real
  baud rate the feed thread drains far faster than bytes arrive.
- On `native_sim` that overflow is easy to trip artificially: a pseudoterminal
  has no baud rate, and native_pty's interrupt-emulation thread runs at
  `K_HIGHEST_THREAD_PRIO` without sleeping. Pace host writes for large uploads
  — the test script does this by default, and `--pace 0` turns it off.
- HTTP has no framing below it, and there is no TCP providing reliability or
  ordering. Bytes must arrive in order, exactly once; a dropped or duplicated
  byte desynchronises the parser, which answers 400 and resets. Over a noisy
  UART, add framing or use a clean link.

## License

Apache-2.0, matching Zephyr. `README.rst` is the upstream-style version of this
document, kept for a possible submission to the Zephyr tree.
