# Zephyr HTTP server over a UART, without a network stack

Serve HTTP on **raw data buffers** — no L2 driver, no IP, no TCP.

The Zephyr HTTP server always talks to its peers through a socket, but it never
looks at what is behind that socket. This project exploits that: the stock,
unmodified server runs with no network stack underneath it, fed raw bytes
straight off a UART.

The only file descriptors in the image are a `socketpair()` and a small custom
listening socket.

## The API

One standalone class, [`src/bare_http.hpp`](src/bare_http.hpp). The output
callback is registered in the constructor; input is a buffer and a size:

```cpp
static void toMyLink(const uint8_t *data, size_t len, void *user)
{
        my_link_write(data, len);        // bytes out of the server
}

static BareHttpServer server(toMyLink);

server.start();
server.input(bytes_from_my_link, n);     // bytes into the server
```

That is the whole surface. `BareHttpServer` knows nothing about UARTs —
[`src/uart_bridge.cpp`](src/uart_bridge.cpp) is a separate class that wires the
two together, and is easy to swap for a USB endpoint, shared memory, or a test
harness.

## The connection is persistent

An HTTP/1.1 request that does not carry `Connection: close` leaves the
connection open; the server loops back to waiting for the next request on the
same socket. **One connection serves requests indefinitely**, which is what
makes a permanently attached UART sensible.

Two details make that robust:

- `CONFIG_HTTP_SERVER_CLIENT_INACTIVITY_TIMEOUT` is raised to its maximum. The
  server tries to drop an idle client with `shutdown()`, which a socketpair does
  not implement, so over this transport the timeout is a no-op anyway — but the
  code does not rely on that quirk.
- If the server does hang up — on a malformed request, say — the next `input()`
  relinks transparently. Any partially delivered request is lost, and the stream
  resynchronises at the next request boundary.

Verified: four requests including a 404, all served over a single connection
with zero reconnects.

## How it works

Only three things bind the HTTP server to a socket, all in
`subsys/net/lib/http/http_server_core.c`:

- the `poll()`/`accept()` loop and the single `recv()` in `http_server_run()`
- the single `send()` in `http_server_sendall()`
- `close()`/`shutdown()` on teardown

Everything above that — HTTP/1 parsing, HTTP/2 framing, resource dispatch — runs
purely on `client->buffer` and never touches a descriptor. So the whole job is to
give the server a descriptor that is not a network socket.

**The listening socket.** `BareHttpServer::socketCreate()` is installed through
`http_service_config::socket_create`, so the server calls it instead of
`zsock_socket()`. It returns a descriptor backed by a custom `socket_op_vtable`
in which `bind()`/`listen()`/`setsockopt()` succeed without doing anything,
`poll()` is backed by a `k_poll_signal`, and `accept()` pops a queued descriptor
instead of waiting for a handshake.

**The connection.** `link()` creates a socketpair and queues one end on the
listening socket, which the server accepts as an ordinary client. An RX thread
pumps the other end into the output callback.

The HTTP server is used entirely as-is: clients live in its internal array, so
the inactivity timers and the assert inside `http_server_sendall()` are
satisfied, and static, dynamic and Websocket resources behave exactly as they
would over TCP.

### Non-blocking is mandatory, not an optimisation

Every `zsock_*()` call takes a per-descriptor mutex and holds it for the whole
call (`VTABLE_CALL` in `subsys/net/lib/sockets/sockets.c`). A blocking `recv()`
parked on the application's end of the socketpair therefore **locks out `send()`
on that same descriptor from any other thread** — a hard deadlock the moment one
connection carries more than one request.

The application end is opened `O_NONBLOCK`, and both directions block in
`zsock_poll()` instead, which does not hold that mutex while waiting. The
server's own end stays blocking, which is what its code expects.

### Threads

- the HTTP server's own thread, created by the subsystem
- an RX thread owned by `BareHttpServer`, polling the connection and invoking the
  output callback
- a feed thread owned by `UartBridge`, because the UART ISR may not block and
  `input()` may

## Building and running

A Zephyr application; needs a Zephyr workspace (tested against v4.4).

```sh
west build -b native_sim /path/to/zephyr-http-server-bare
./build/zephyr/zephyr.exe
```

At boot it injects three keep-alive requests on the UART's own connection, so
the mechanism is visible without a terminal attached:

```
<inf> uart_bridge: UART uart_1 wired to the HTTP server
<inf> net_http_server_bare: --> request 1: feeding 30 bytes in two chunks
<inf> net_http_server_bare:     connection still up: yes
<inf> net_http_server_bare: --> request 2: feeding 36 bytes in two chunks
<inf> net_http_server_bare:     connection still up: yes
<inf> net_http_server_bare: --> request 3: feeding 36 bytes in two chunks
<inf> net_http_server_bare:     connection still up: yes
<inf> net_http_server_bare: Self-test done, now serving the UART forever
```

Each request is handed over in **two separate calls** on purpose: the server
treats its input as a byte stream, so requests may be split across as many
buffers as the transport produces.

`native_sim` prints the pseudoterminal backing `uart1` on startup. Attach to it
to drive the server by hand — put the terminal in **raw mode** first, or the
default line discipline (ICRNL, OPOST, ECHO) will mangle the HTTP byte stream:

```
uart_1 connected to pseudotty: /dev/pts/9
```

Set `CONFIG_APP_SELFTEST=n` to skip the injected requests and serve only what
arrives on the UART.

The UART is selected by the `http-uart` devicetree alias; the supplied
`native_sim` overlay points it at `uart1` so the console keeps `uart0`.

## Framing

There is no framing below HTTP — and there never was. TCP does not provide
message boundaries either; HTTP self-delimits at the application layer: the
request head ends at `CRLFCRLF`, and body length comes from `Content-Length` or
`Transfer-Encoding: chunked`.

`http_parser_execute()` is a resumable, byte-at-a-time parser whose state lives
in `client->parser` across calls, and the core loop compacts leftover bytes to
the front of the buffer after every pass. Partial requests just work.

What TCP *was* providing is **reliability and ordering**, and going bare makes
that your problem. Bytes must arrive in order, exactly once, with no gaps; a
dropped or duplicated byte desynchronises the parser, which returns `-EBADMSG`
and drops the connection. Over a socketpair that is free. Over a noisy UART it
is not.

## Configuration notes

`CONFIG_NETWORKING` and `CONFIG_NET_SOCKETS` are still required, because the
server is written against the socket API — but `CONFIG_NET_NATIVE=n` removes the
IP stack, connection tracking and packet pools from the build entirely.

`CONFIG_NET_IPV4` stays enabled for one reason only: `http_server_init()` fills
in a `sockaddr` *before* it calls the `socket_create` hook, and gives up if
neither address family is configured. The address is never used. For the same
reason the service is declared with a non-zero port — port 0 means "ephemeral",
which would make the server call `getsockname()` on the bare listener.

### Buffer sizes

`CONFIG_HTTP_SERVER_CLIENT_BUFFER_SIZE` is **not** a limit on request head size
under HTTP/1. The parser consumes everything it is given and the buffer is
compacted every pass, so a head much larger than this buffer parses fine
(verified with a 1464-byte head on a 512-byte buffer). It is a throughput knob —
though it *does* become a hard limit if you enable HTTP/2, where a whole frame
must be buffered before it can be handled.

The actual bound on a request head is `CONFIG_HTTP_SERVER_MAX_URL_LENGTH`
(default 256, max 2048), since `on_url()` accumulates the URL into a fixed
buffer. With `CONFIG_HTTP_SERVER_CAPTURE_HEADERS`, individual captured header
fields are bound by `CONFIG_HTTP_SERVER_MAX_HEADER_LEN`.

## Things to watch out for

- The output callback runs on the RX thread, not the server thread. Never call
  `input()` from it.
- Never assume a fixed callback size: the payload is whatever was buffered, so a
  138-byte response arrives as one 138-byte call. If your link needs full frames,
  accumulate them yourself.
- `UartBridge` transmits with `uart_poll_out()`, which busy-waits per byte. Fine
  at sane baud rates; switch to interrupt-driven TX with a second ring buffer if
  you push large responses at high speed.
- If the UART RX ring overflows, bytes are dropped and the request stream
  desynchronises. The bridge logs an error when that happens.
- One `BareHttpServer` instance at a time: the listening socket is a
  process-wide singleton, so a second concurrent `start()` returns `-EEXIST`.
  This is a single-client design by construction.

## License

Apache-2.0, matching Zephyr. `README.rst` is the upstream-style version of this
document, kept for a possible submission to the Zephyr tree.
