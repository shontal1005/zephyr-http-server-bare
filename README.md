# Zephyr HTTP server without a network stack

Run Zephyr's HTTP server on **raw data buffers** — no L2 driver, no IP, no TCP.

The Zephyr HTTP server always talks to its peers through a socket, but it never
looks at what is behind that socket. This sample exploits that: the stock server
runs completely unmodified, and the application hands it request bytes directly.

The only file descriptors in the image are a `socketpair()` and a small custom
listening socket.

## The API

Two entry points, both in [`src/bare_transport.h`](src/bare_transport.h):

```c
/* data INTO the server */
int bare_http_input(struct bare_conn *conn, const void *data, size_t len);

/* data OUT of the server */
typedef void (*bare_http_out_cb_t)(const uint8_t *data, size_t len, void *user_data);
```

Both are placeholders. Replace their bodies with reads and writes on whatever
carries your data — a UART, a USB endpoint, shared memory with another core, or
a test harness — and nothing else changes.

## How it works

Only three things bind the HTTP server to a socket, all in
`subsys/net/lib/http/http_server_core.c`:

- the `poll()`/`accept()` loop and the single `recv()` in `http_server_run()`
- the single `send()` in `http_server_sendall()`
- `close()`/`shutdown()` on teardown

Everything above that — HTTP/1 parsing, HTTP/2 framing, resource dispatch — runs
purely on `client->buffer` and never touches a descriptor. So the whole job is to
give the server a descriptor that is not a network socket.

[`src/bare_transport.c`](src/bare_transport.c) does that in two parts.

**The listening socket.** `bare_http_listener_create()` is installed through
`http_service_config.socket_create`, so the server calls it instead of
`zsock_socket()`. It returns a descriptor backed by a custom `socket_op_vtable`
in which:

- `bind()` and `listen()` succeed without doing anything — the server abandons a
  service if they fail, but there is no address space here
- `setsockopt()` succeeds so the `SO_REUSEADDR` call does not abort the service
- `poll()` is backed by a `k_poll_signal`, raised when the app queues a connection
- `accept()` pops a queued descriptor instead of waiting for a handshake

**The connection.** `bare_http_conn_open()` creates a socketpair, queues one end
on the listening socket — which the server then accepts as an ordinary client —
and keeps the other. A small thread pumps that end into the app callback.

The HTTP server is used entirely as-is: clients live in its internal array, so
the inactivity timers and the assert inside `http_server_sendall()` are
satisfied, and static, dynamic and Websocket resources all behave exactly as they
would over TCP.

## Building and running

This is a Zephyr application; it needs a Zephyr workspace (tested against v4.4).

```sh
west build -b native_sim /path/to/zephyr-http-server-bare
./build/zephyr/zephyr.exe
```

The sample issues two requests, one against a static resource and one against a
dynamic one, and prints the raw bytes the server produced:

```
--> feeding 49 bytes in two chunks
<-- 138 bytes from the HTTP server:
HTTP/1.1 200 OK
Content-Type: text/html
Content-Length: 74

<html><body><h1>Zephyr HTTP server, no network attached</h1></body></html>
<-- connection closed by the server
--> feeding 55 bytes in two chunks
<-- 105 bytes from the HTTP server:
HTTP/1.1 200
Transfer-Encoding: chunked
Content-Type: application/json

12
{"uptime_ms":110}

0

<-- connection closed by the server
Done
```

Each request is handed over in **two separate calls** on purpose. The server
treats its input as a byte stream, so requests may be split across as many
buffers as the transport produces, and responses may come back over several
callback invocations.

## Framing

There is no framing below HTTP — and there never was. TCP does not provide
message boundaries either; HTTP has always self-delimited at the application
layer:

- the request head is terminated by `CRLFCRLF`
- body length comes from `Content-Length` or `Transfer-Encoding: chunked`

`http_parser_execute()` is a resumable, byte-at-a-time parser whose state lives
in `client->parser` across calls, and the core loop compacts leftover bytes to
the front of the buffer after every pass. Partial requests therefore just work.

What TCP *was* providing is **reliability and ordering**, and going bare makes
that your problem. Bytes must arrive in order, exactly once, with no gaps; a
dropped or duplicated byte desynchronises the parser, which returns `-EBADMSG`
and drops the connection. Over a socketpair or shared memory that is free. Over
a raw UART or a radio link it is not.

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

- `bare_http_input()` blocks once the socketpair buffer
  (`CONFIG_NET_SOCKETPAIR_BUFFER_SIZE`) is full, and so does the server writing a
  response. **Drain the output from a different thread than the one feeding
  input**, as this sample does, or the two deadlock on a response larger than the
  buffer.
- The output callback runs on the connection RX thread, not the server thread.
- Never assume a fixed callback size: `recv()` returns whatever is buffered, so a
  138-byte response arrives as one 138-byte call regardless of `BARE_RX_BUF_SIZE`.
  If your link needs full frames, accumulate them yourself.
- Raising the connection count means raising `BARE_MAX_CONN` and
  `BARE_MAX_PENDING` in `src/bare_transport.c`, plus
  `CONFIG_HTTP_SERVER_MAX_CLIENTS`, `CONFIG_NET_SOCKETPAIR_MAX` and
  `CONFIG_ZVFS_OPEN_MAX`, together.

## License

Apache-2.0, matching Zephyr. `README.rst` is the upstream-style version of this
document, kept for a possible submission to the Zephyr tree.
