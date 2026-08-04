.. zephyr:code-sample:: http-server-bare
   :name: HTTP server over a UART, without a network stack
   :relevant-api: http_server http_service socket_apis uart_interface

   Serve HTTP over raw data buffers, with no L2, IP or TCP involved.

Overview
********

The Zephyr HTTP server always talks to its peers through a socket, but it never
looks at what is behind that socket. This sample exploits that: it runs the
stock HTTP server with **no network stack at all** underneath it, and feeds it
raw bytes straight off a UART.

There is no L2 driver, no IP and no TCP in the image. The only file descriptors
in play are a :c:func:`socketpair` and a small custom listening socket.

The application-facing surface is a single C++ class, ``RawHttpServer``:

.. code-block:: cpp

   static void toMyLink(const uint8_t *data, size_t len, void *user)
   {
           my_link_write(data, len);        /* bytes out of the server */
   }

   static RawHttpServer server(toMyLink);

   server.start();
   server.input(bytes_from_my_link, n);     /* bytes into the server */

The output callback is registered through the constructor and ``input()`` takes
a buffer and a size. That is the whole API; ``RawHttpServer`` knows nothing
about UARTs. :file:`src/uart_bridge.cpp` is a separate class that connects the
two, and is easy to replace with a USB endpoint, shared memory or a test
harness.

The connection is persistent
****************************

An HTTP/1.1 request that does not carry ``Connection: close`` leaves the
connection open, and the server loops back to waiting for the next request on
the same socket. One connection therefore serves requests indefinitely - which
is what makes a permanently attached UART sensible.

Two details make that robust:

* ``CONFIG_HTTP_SERVER_CLIENT_INACTIVITY_TIMEOUT`` is raised to its maximum.
  The server tries to drop an idle client with ``shutdown()``, which a
  socketpair does not implement, so over this transport the timeout is a no-op
  anyway - but the sample does not rely on that quirk.
* If the server does hang up - on a malformed request, for instance - a
  following ``input()`` re-establishes the connection. Recovery is not seamless:
  the buffer that races the close is lost, and measurement shows one request
  goes missing before the relink takes effect. The request after it succeeds. A
  partially delivered buffer is dropped rather than straddling two connections,
  which would arrive as garbage on both.

How it works
************

Only three things bind the HTTP server to a socket, all of them in
:file:`subsys/net/lib/http/http_server_core.c`:

* the ``poll()``/``accept()`` loop and the single ``recv()`` in
  ``http_server_run()``;
* the single ``send()`` in ``http_server_sendall()``;
* ``close()``/``shutdown()`` on teardown.

Everything above that - HTTP/1 parsing, HTTP/2 framing, resource dispatch - runs
purely on ``client->buffer`` and never touches a descriptor. So the whole job is
to give the server a descriptor that is not a network socket.

**The listening socket.** ``RawHttpServer::socketCreate()`` is installed
through :c:member:`http_service_config.socket_create`, so the server calls it
instead of :c:func:`zsock_socket`. It returns a descriptor backed by a custom
``socket_op_vtable`` in which ``bind()`` and ``listen()`` succeed without doing
anything, ``setsockopt()`` succeeds so the ``SO_REUSEADDR`` call does not abort
the service, ``poll()`` is backed by a :c:struct:`k_poll_signal`, and
``accept()`` pops a queued descriptor instead of waiting for a handshake.

**The connection.** ``link()`` creates a socketpair and queues one end on the
listening socket, which the server then accepts as an ordinary client. A small
RX thread pumps the other end into the output callback.

The HTTP server is used completely unmodified: clients live in its internal
client array, the inactivity timers run, and static, dynamic and Websocket
resources all behave exactly as they would over TCP.

Non-blocking is mandatory, not an optimisation
==============================================

Every ``zsock_*()`` call takes a per-descriptor mutex and holds it for the whole
call (``VTABLE_CALL`` in :file:`subsys/net/lib/sockets/sockets.c`). A blocking
``recv()`` parked on the application's end of the socketpair therefore locks out
``send()`` on that same descriptor from any other thread - a hard deadlock the
moment one connection carries more than one request.

The application end is consequently opened ``O_NONBLOCK``, so nothing blocks
inside that mutex. The RX side then waits in :c:func:`zsock_poll` with
``POLLIN``, which does not hold it. The TX side deliberately does **not** use
``POLLOUT``: socketpair's POLLOUT poll-prepare takes the *peer's* semaphore with
``K_FOREVER`` while ``zvfs_poll_internal()`` holds the per-fd mutex, so if the
server is itself parked in a blocking write, all three threads deadlock.
``input()`` therefore retries a non-blocking ``send()`` on a short sleep, which
gives up with ``EAGAIN`` instead of waiting on that semaphore.

The server's own end stays blocking, which is what its code expects.

Threads
=======

* the HTTP server's own thread, created by the subsystem;
* an RX thread owned by ``RawHttpServer``, which polls the connection and
  invokes the output callback;
* a feed thread owned by ``UartBridge``, because the UART ISR may not block and
  ``input()`` may.

Configuration notes
*******************

``CONFIG_NETWORKING`` and ``CONFIG_NET_SOCKETS`` are still needed because the
server is written against the socket API, but ``CONFIG_NET_NATIVE=n`` removes
the IP stack, connection tracking and packet pools from the build.

``CONFIG_NET_IPV4`` stays enabled for one reason only: ``http_server_init()``
fills in a ``sockaddr`` *before* it calls the ``socket_create`` hook, and bails
out if neither address family is configured. The address is never used. For the
same reason the service is declared with a non-zero port - a zero port means
"ephemeral", which would make the server call ``getsockname()``.

Buffer sizes
============

``CONFIG_HTTP_SERVER_CLIENT_BUFFER_SIZE`` is **not** a limit on request head
size under HTTP/1. The parser consumes everything it is given and the buffer is
compacted after every pass, so a head much larger than this buffer parses fine.
It is a throughput knob - though it does become a hard limit if HTTP/2 is
enabled, where a whole frame must be buffered before it can be handled.

The actual bound on a request head is ``CONFIG_HTTP_SERVER_MAX_URL_LENGTH``
(default 256, max 2048), because ``on_url()`` accumulates the URL into a fixed
buffer. With ``CONFIG_HTTP_SERVER_CAPTURE_HEADERS``, individual captured header
fields are bound by ``CONFIG_HTTP_SERVER_MAX_HEADER_LEN``.

Wiring
******

The UART is selected by the ``http-uart`` devicetree alias. On ``native_sim``
the supplied overlay points it at ``uart1`` so that the console keeps ``uart0``:

.. code-block:: devicetree

   / {
           aliases {
                   http-uart = &uart1;
           };
   };

   &uart1 {
           status = "okay";
   };

Building and running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/net/sockets/http_server_bare
   :board: native_sim
   :goals: build run
   :compact:

At boot the sample injects three keep-alive requests on the UART's own
connection, so the mechanism is visible without a terminal attached:

.. code-block:: console

   <inf> uart_bridge: UART uart_1 wired to the HTTP server
   <inf> net_http_server_bare: --> request 1: feeding 30 bytes in two chunks
   <inf> net_http_server_bare:     connection still up: yes
   <inf> net_http_server_bare: --> request 2: feeding 36 bytes in two chunks
   <inf> net_http_server_bare:     connection still up: yes
   <inf> net_http_server_bare: --> request 3: feeding 36 bytes in two chunks
   <inf> net_http_server_bare:     connection still up: yes
   <inf> net_http_server_bare: Self-test done, now serving the UART forever

Note that each request is handed over in two separate calls. The server treats
its input as a byte stream, so requests may be split across as many buffers as
the transport happens to produce, and responses may come back over several
callback invocations.

``native_sim`` prints the pseudoterminal backing ``uart1`` on startup. Attach to
it to drive the server by hand - remember to put the terminal in raw mode, since
the default line discipline would mangle an HTTP byte stream:

.. code-block:: console

   uart_1 connected to pseudotty: /dev/pts/9

Set ``CONFIG_APP_SELFTEST=n`` to skip the injected requests and serve only what
arrives on the UART.

Things to watch out for
***********************

* The output callback runs on the RX thread, not on the HTTP server thread.
  Never call ``input()`` from it.
* Never assume a fixed callback size: the payload is whatever was buffered, so a
  138-byte response arrives as one 138-byte call regardless of buffer sizes. If
  your link needs full frames, accumulate them yourself.
* ``UartBridge`` transmits with :c:func:`uart_poll_out`, which busy-waits per
  byte. That is fine at sane baud rates; switch to interrupt-driven TX with a
  second ring buffer if you push large responses at high speed.
* If the UART RX ring overflows, bytes are dropped and the request stream
  desynchronises. The bridge logs an error when that happens.
* HTTP has no framing below it, and TCP is no longer providing reliability or
  ordering either. Bytes must arrive in order, exactly once, with no gaps; a
  dropped or duplicated byte makes the parser return ``-EBADMSG`` and the server
  close the connection. Over a socketpair that is free, over a noisy UART it is
  not.
