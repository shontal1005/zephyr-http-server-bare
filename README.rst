.. zephyr:code-sample:: http-server-bare
   :name: HTTP file server over a UART, without a network stack
   :relevant-api: http_parser file_system_api uart_interface

   Download files over a UART, with no L2, IP or TCP involved.

Overview
********

This sample serves files over HTTP/1.1 with **no network stack at all**: no L2
driver, no IP, no TCP, no sockets. The only piece of Zephyr's networking tree
in the image is the bundled HTTP request parser library
(:kconfig:option:`CONFIG_HTTP_PARSER`). ``RawHttpServer`` drives that parser
directly: request packets go in through a method, response bytes come out
through a callback, and the file I/O happens in between. GET downloads a file
out of an already-mounted directory - that is the whole surface, and any other
method is answered with 405.

The application-facing surface is a single C++ class, ``RawHttpServer``:

.. code-block:: cpp

   NET_BUF_POOL_DEFINE(my_pool, 8, 1024, 0, NULL);

   static void to_my_link(const uint8_t *data, size_t len, void *user)
   {
           my_link_write(data, len);                /* bytes out of the server */
   }

   /* the URL is the filesystem path: a mount at /lfs serves "GET /lfs/..." */
   static RawHttpServer server(to_my_link);

   struct net_buf *packet = net_buf_alloc(&my_pool, K_FOREVER);
   net_buf_add_mem(packet, bytes_from_my_link, n);
   server.enqueue_packet(packet);           /* bytes into the server */

Constructing the server starts its thread; there is nothing else to start.
Packets carry **arbitrary chunks of the request byte stream** - the server
does the framing. ``enqueue_packet()`` only queues - it is safe from any
thread or ISR - and the server's thread consumes packets in arrival order,
answering each request through the output callback as its head completes:
requests are never handled in parallel. A request may be split across any
number of packets - its head is assembled in an internal buffer of
``CONFIG_RAW_HTTP_HEAD_MAX`` bytes. The server answers **one request per
client round trip**: once a request is answered, everything already received
beyond it is dropped, so a client must read the response before sending its
next request. Bodies are **never** buffered: a refused PUT's body is counted
down by ``Content-Length`` and discarded as it streams past, keeping the
stream aligned. A zero-length packet carries no bytes and is dropped
silently.
Responses are staged in an internal buffer of ``CONFIG_RAW_HTTP_FILE_CHUNK``
bytes: the bytes handed to the output callback are valid only during the
call, so copy or transmit them before returning.
``RawHttpServer`` knows nothing about UARTs. :file:`src/uart_bridge.cpp` is a
separate class that connects the two, and is easy to replace with a USB
endpoint, shared memory or a test harness.

Using it as a library
*********************

The repository is also a Zephyr module (:file:`zephyr/module.yml`). The
library is ``RawHttpServer`` alone, in :file:`lib/` and :file:`include/`;
everything under :file:`src/` - the application and the UART bridge - is
sample code showing one way to feed it. Add the repository to
``ZEPHYR_EXTRA_MODULES`` or to a west manifest, then enable:

.. code-block:: cfg

   CONFIG_RAW_HTTP_SERVER=y

``RAW_HTTP_SERVER`` selects ``CPP``, ``FILE_SYSTEM``, ``HTTP_PARSER``,
``NET_BUF`` and ``NETWORKING``. The URL bound, head-assembly bound, download
chunk size, thread stack size and priority, and log level are Kconfig options
under the ``RAW_HTTP_SERVER`` menu.

Files
*****

The URL **is** the filesystem path, resolved through whatever filesystems are
**already mounted** - the server never mounts anything itself, and Zephyr's
VFS is the validator: a URL outside every mount, a ``..`` climbing above a
filesystem root (littlefs refuses it) or a directory simply fails the
filesystem call and is answered with 404. The sample mounts a littlefs at
``/lfs``:

.. code-block:: console

   GET /lfs/report.bin      download the file at /lfs/report.bin

Downloads stream in chunks staged in the server's internal buffer -
``CONFIG_RAW_HTTP_FILE_CHUNK`` bytes per output callback - so file size is not
bounded by RAM. The size comes from :c:func:`fs_stat`, so
responses carry a plain ``Content-Length`` - no chunked encoding. A missing
file returns 404, a ``..`` in the name is rejected, and the stream keeps
serving.

No connection to lose
*********************

HTTP/1.1 keep-alive falls out of the design instead of being engineered in:
there is no connection object anywhere, just a reassembled byte stream.
Every request gets exactly one response - per request, not per packet: 200
with the file, 404 (nothing servable at that path), 405 (not a GET), 414
(URL longer than ``CONFIG_RAW_HTTP_URL_MAX``), 431 (head outgrew
``CONFIG_RAW_HTTP_HEAD_MAX``), or 400 (malformed bytes, a chunked body, an
upgrade or CONNECT - no other protocol is spoken here). An incomplete
request earns **silence**, not an error: the head completes whenever its
bytes arrive, like on any HTTP server. Pipelining is not: one request is
answered per round trip, and a request sent ahead of the previous response
is dropped, never answered - its tail may later parse alone as garbage and
earn a stray 400, from which the stream self-heals. Unparseable bytes
likewise cost a single 400 and a buffer reset; the stream self-heals at the
next parseable request (the refused request's own tail may earn one
follow-up 400), so the client's request/response accounting never
desynchronises for long.

How it works
************

Each packet is consumed loop-free: bytes still owed to the previously
answered request's body are discarded off the front, what fits of the rest
is appended to the head assembly buffer, and **one** parse attempt is made -
on success the request is answered and the packet's remainder is dropped
(discounted against the body debt first, since it may be the answered
request's own body), so at most one response leaves per packet. Each attempt
parses the buffer from the start with a **freshly reinitialised parser**, so
no parser state ever spans packets. Only two callbacks are registered:

* ``on_url`` captures the URL (a URL split across packets is simply re-seen
  whole on the next attempt, since every attempt re-parses from the buffer
  start), and fails the parse if it outgrows ``CONFIG_RAW_HTTP_URL_MAX``
  rather than truncating silently;
* ``on_headers_complete`` deliberately returns -1 to **halt the parser** at
  the end of the head - the head is everything a GET-only server needs, so
  the body is never parsed (0, 1 and 2 are magic values to this callback;
  -1 is a plain error).

That halt makes the parser's errno the entire verdict:
``HPE_CB_headers_complete`` is the *success* code, ``HPE_OK`` honestly means
the head is incomplete - wait silently for more bytes - ``HPE_CB_url`` is an
overlong URL (414), and anything else is malformed bytes (400, and the buffer
is dropped since boundaries are unknown). After a complete head: an upgrade
or CONNECT gets a 400 and a buffer reset (what follows is not HTTP), a
chunked body gets a 400 too (no predeclared length to skip past), a non-GET
gets its 405 as soon as the head completes - even before its body arrives -
and the body is then discarded by ``Content-Length`` as it streams past; a
GET has its query string cut off and the URL goes to the VFS - a failed
:c:func:`fs_stat` or :c:func:`fs_open`, or a directory, is a 404, and a 200
streams the file back. A head that outgrows a full assembly buffer is
answered 431 and the buffer reset.

There are no sockets and no state machine beyond the parser's own. The class
owns one thread and one packet queue - and no mutex: the
server thread is the only thing touching request state, ``enqueue_packet()``
only touches the queue (a ``k_fifo``, safe from threads and ISRs), and the
single-consumer queue is what serialises requests. The sample adds one more
thread, the ``UartBridge`` feed thread: it wraps whatever the RX ring holds
in packets as bytes arrive - no framing, the server reassembles - and exists
because allocating a packet may block on an exhausted pool, which the UART
ISR may not do.

Configuration notes
*******************

``RAW_HTTP_SERVER`` selects :kconfig:option:`CONFIG_NETWORKING` for one reason
only: the parser library's Kconfig lives under the networking menu. ``select``
forces just that one symbol - sub-options such as
:kconfig:option:`CONFIG_NET_NATIVE` follow their own defaults, so the module's
Kconfig flips ``NET_NATIVE`` to ``default n`` (module Kconfig files are
sourced before the subsystem tree, and the first satisfied default wins). The
result is no IP stack, no sockets and no TCP in the image; an application that
also wants real networking sets ``CONFIG_NET_NATIVE=y`` back.

One caveat: board :file:`Kconfig.defconfig` files are sourced before modules
and deliberately outrank them. ``native_sim`` switches Ethernet on whenever
networking is in the build, so the sample's :file:`prj.conf` carries an
explicit ``CONFIG_NET_L2_ETHERNET=n`` - expect the same on any board that
self-enables its network driver.

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
   :zephyr-app: samples/net/http_server_bare
   :board: native_sim
   :goals: build run
   :compact:

``native_sim`` has nothing mounted, so the sample mounts a littlefs on the
flash simulator at ``/lfs`` and seeds two files into it: ``hello.txt`` and
``big.bin``, a 5035-byte position-dependent pattern that exceeds
``CONFIG_RAW_HTTP_FILE_CHUNK`` and so downloads over several chunks (the test
suite regenerates it byte for byte). The board overlay also grows
``storage_partition`` from its stock 16 KiB to 1 MiB - littlefs metadata
consumes most of 16 KiB, and seeding the demo files would otherwise fail with
``-ENOSPC``. A real board would have mounted its storage during boot and
simply passed the path to the constructor.

At boot the sample injects five requests - a 200, a 404, a 405, a 400 and a
split 200 - so the mechanism is visible without a terminal attached:

.. code-block:: console

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

The five demonstrate a download of the seeded file (200), a missing file
(404), a PUT whose body is skipped (405), bytes that are not HTTP (a single
400, then the stream heals), and a GET deliberately split across two packets
and reassembled (200). Set ``CONFIG_APP_SELFTEST=n`` to skip the injected
requests and serve only what arrives on the UART.

``native_sim`` prints the pseudoterminal backing ``uart1`` on startup. Attach
to it to drive the server by hand - remember to put the terminal in raw mode,
since the default line discipline would mangle an HTTP byte stream:

.. code-block:: console

   uart_1 connected to pseudotty: /dev/pts/9

Testing against a real board
****************************

:file:`tests/http_over_serial_test.py` drives the whole thing over a serial
device given as an argument:

.. code-block:: console

   ./tests/http_over_serial_test.py /dev/ttyUSB0
   ./tests/http_over_serial_test.py /dev/ttyACM0 --baud 921600

It downloads the seeded ``hello.txt``, downloads the multi-chunk ``big.bin``
and compares it byte for byte against the regenerated pattern, checks that a
PUT is refused with 405 and that a missing file returns 404, sends a request
split into two writes to prove it is reassembled, and re-downloads to prove
the stream survived all of it. Exit status is 0 on pass, 1 on failure.
``native_sim`` works through the same path: pass the pseudoterminal it prints
for ``uart1`` instead of a real device.

pyserial is used when present and is required for ``--baud``; without it the
device is opened raw, which is all a pseudoterminal needs.

Things to watch out for
***********************

* The output callback runs on the server's thread, and the bytes it receives
  are valid only during the call: copy or transmit them before returning.
  Blocking there is fine - it is the natural flow control against a slow
  link.
* Never assume a fixed callback size: one response may arrive over several
  calls, and ``len`` is whatever the server produced in one go.
* ``Expect: 100-continue`` is not implemented - but a non-GET is answered as
  soon as its head completes, so a client pausing for the 100 gets its 405
  instead, and its body is discarded when (and if) it arrives.
* A request carrying ``Connection: close`` is served, but there is no
  connection to close - the stream simply keeps serving.
* ``UartBridge`` transmits with :c:func:`uart_poll_out`, which busy-waits per
  byte. That is fine at sane baud rates; switch to interrupt-driven TX if you
  push large responses at high speed.
* If the UART RX ring (``UART_BRIDGE_RING_SIZE``) overflows, bytes are
  dropped and the bridge logs the exact count. Nothing else is needed: the
  mangled request answers for itself with a 400, and the stream self-heals
  at the next request boundary. At any real baud rate the feed thread drains
  far faster than bytes arrive.
* On ``native_sim`` that overflow is easy to trip artificially: a
  pseudoterminal has no baud rate, and native_pty's interrupt-emulation thread
  runs at ``K_HIGHEST_THREAD_PRIO`` without sleeping. The client paces its
  writes by default, and ``--pace 0`` turns it off.
* HTTP has no framing below it, and there is no TCP providing reliability or
  ordering. Bytes must arrive in order, exactly once, with no gaps; a dropped
  or duplicated byte mangles its request, which costs a single 400 and a
  reset - the stream heals at the next parseable request. Over a noisy UART,
  add framing or use a clean link.
