.. zephyr:code-sample:: http-server-bare
   :name: HTTP server without a network stack
   :relevant-api: http_server http_service socket_apis

   Drive the HTTP server with raw data buffers, with no L2, IP or TCP involved.

Overview
********

The Zephyr HTTP server always talks to its peers through a socket, but it never
looks at what is behind that socket. This sample exploits that: it runs the
stock HTTP server with **no network stack at all** underneath it, and feeds it
raw request bytes from the application instead.

There is no L2 driver, no IP and no TCP in the image. The only file descriptors
in play are a :c:func:`socketpair` and a small custom listening socket.

The application only ever sees two entry points, both in
:file:`src/bare_transport.h`:

* :c:func:`bare_http_input` — push raw request bytes *into* the server.
* :c:type:`bare_http_out_cb_t` — a callback delivering the raw response bytes
  coming *out* of the server.

Replace the bodies of those two placeholders with reads and writes on whatever
carries your data: a UART, a USB endpoint, shared memory with another core, or
a unit-test harness.

How it works
************

Only three things bind the HTTP server to a socket, all of them in
:file:`subsys/net/lib/http/http_server_core.c`:

* the ``poll()``/``accept()`` loop and the single ``recv()`` in
  ``http_server_run()``;
* the single ``send()`` in ``http_server_sendall()``;
* ``close()``/``shutdown()`` on teardown.

Everything above that — HTTP/1 parsing, HTTP/2 framing, resource dispatch — runs
purely on ``client->buffer`` and never touches a descriptor. So the whole job is
to give the server a descriptor that is not a network socket.

:file:`src/bare_transport.c` does that in two parts.

**The listening socket.** ``bare_http_listener_create()`` is installed through
:c:member:`http_service_config.socket_create`, so the server calls it instead of
:c:func:`zsock_socket`. It returns a descriptor backed by a custom
``socket_op_vtable`` in which:

* ``bind()`` and ``listen()`` succeed without doing anything — the server gives
  up on a service if they fail, but there is no address space here;
* ``setsockopt()`` succeeds so that the ``SO_REUSEADDR`` call does not abort the
  service;
* ``poll()`` is backed by a :c:struct:`k_poll_signal`, raised when the
  application queues a connection;
* ``accept()`` pops a queued descriptor instead of waiting for a handshake.

**The connection.** ``bare_http_conn_open()`` creates a socketpair, queues one
end on the listening socket — which the server then accepts as an ordinary
client — and keeps the other end. A small thread pumps that end into the
application callback.

The upshot is that the HTTP server is used completely unmodified: clients live
in its internal client array, the inactivity timers run, and static, dynamic and
Websocket resources all behave exactly as they would over TCP.

Configuration notes
*******************

``CONFIG_NETWORKING`` and ``CONFIG_NET_SOCKETS`` are still needed because the
server is written against the socket API, but ``CONFIG_NET_NATIVE=n`` removes
the IP stack, connection tracking and packet pools from the build.

``CONFIG_NET_IPV4`` stays enabled for one reason only: ``http_server_init()``
fills in a ``sockaddr`` *before* it calls the ``socket_create`` hook, and bails
out if neither address family is configured. The address is never used. For the
same reason the service is declared with a non-zero port — a zero port means
"ephemeral", which would make the server call ``getsockname()``.

Building and running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/net/sockets/http_server_bare
   :board: native_sim
   :goals: build run
   :compact:

The sample issues two requests, one against a static resource and one against a
dynamic one, and prints the raw bytes the server produced:

.. code-block:: console

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

Note that each request is handed over in two separate calls. The server treats
its input as a byte stream, so requests may be split across as many buffers as
the transport happens to produce, and responses may come back in several
callback invocations.

Things to watch out for
***********************

* :c:func:`bare_http_input` blocks once the socketpair buffer
  (``CONFIG_NET_SOCKETPAIR_BUFFER_SIZE``) is full, and so does the server when
  it writes a response. Drain the output from a different thread than the one
  feeding input, as this sample does, or the two can deadlock on a response
  larger than the buffer.
* The output callback runs on the connection RX thread, not on the HTTP server
  thread.
* Raising the number of concurrent connections means raising ``BARE_MAX_CONN``
  and ``BARE_MAX_PENDING`` in :file:`src/bare_transport.c`,
  ``CONFIG_HTTP_SERVER_MAX_CLIENTS``, ``CONFIG_NET_SOCKETPAIR_MAX`` and
  ``CONFIG_ZVFS_OPEN_MAX`` together.
