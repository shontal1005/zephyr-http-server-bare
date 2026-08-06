.. zephyr:code-sample:: http-server-bare
   :name: HTTP file server over a UART, without a network stack
   :relevant-api: http_parser file_system_api uart_interface

   Download and upload files over a UART, with no L2, IP or TCP involved.

Overview
********

This sample serves files over HTTP/1.1 with **no network stack at all**: no L2
driver, no IP, no TCP, no sockets. The only piece of Zephyr's networking tree
in the image is the bundled HTTP request parser library
(:kconfig:option:`CONFIG_HTTP_PARSER`). ``RawHttpServer`` drives that parser
directly: request bytes go in through a method, response bytes come out through
a callback, and the parser callbacks do the file I/O in between. GET downloads
a file out of an already-mounted directory, PUT and POST upload one into it.

The application-facing surface is a single C++ class, ``RawHttpServer``:

.. code-block:: cpp

   NET_BUF_POOL_DEFINE(my_pool, 8, 1024, 0, NULL);

   static void to_my_link(const uint8_t *data, size_t len, void *user)
   {
           my_link_write(data, len);                /* bytes out of the server */
   }

   /* second argument is the already-mounted directory files live in */
   static RawHttpServer server(to_my_link, "/lfs");

   struct net_buf *packet = net_buf_alloc(&my_pool, K_FOREVER);
   net_buf_add_mem(packet, bytes_from_my_link, n);
   server.enqueue_packet(packet);           /* bytes into the server */

Constructing the server starts its thread; there is nothing else to start.
``enqueue_packet()`` only queues - it is safe from any thread or ISR - and the
server's thread parses each packet in arrival order, emitting the response
through the output callback before taking the next: requests are never handled
in parallel. Responses are staged in an internal buffer of
``CONFIG_RAW_HTTP_FILE_CHUNK`` bytes, never in the request's packet: the bytes
handed to the output callback are valid only during the call, so copy or
transmit them before returning. Because the packet is never reused for the
response, one packet may carry several pipelined requests, each answered in
order as the parser reaches it.
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
``NET_BUF`` and ``NETWORKING``. The URL prefix, URL/path bounds, download chunk
size, thread stack size and priority, and log level are Kconfig options under
the ``RAW_HTTP_SERVER`` menu.

Files
*****

The constructor takes the path of a directory that is **already mounted** - the
server never mounts anything itself. Files are served under ``/files/``:

.. code-block:: console

   GET  /files/report.bin      download a file from <fs_root>/report.bin
   PUT  /files/report.bin      upload the request body to that path
   POST /files/report.bin      same as PUT

Downloads stream in chunks staged in the server's internal buffer -
``CONFIG_RAW_HTTP_FILE_CHUNK`` bytes per output callback - so file size is not
bounded by RAM. The size comes from :c:func:`fs_stat`, so
responses carry a plain ``Content-Length`` - no chunked encoding. Uploads work
the same way in reverse: body fragments are written to the file straight out of
the request packet as the parser delivers them, so they are not bounded
either. A missing file returns 404, a ``..`` in the name is rejected, and the
stream keeps serving.

No connection to lose
*********************

HTTP/1.1 keep-alive falls out of the design instead of being engineered in:
there is no connection object anywhere, just a resumable parser on a byte
stream. Requests may be split across any number of packets. A malformed request
is answered with a single 400 and the parser resets; the rest of that broken
request keeps arriving as more unparseable packets, and those are discarded
silently - one response per failure burst, so the client's request/response
accounting never desynchronises. Upgrade and CONNECT requests are answered 400
too (no other protocol is spoken here). Pipelining works: one packet may carry
several complete requests, and each is answered in order as the parser reaches
it - responses are staged in the server's own buffer, never in the packet
being parsed.

How it works
************

:c:func:`http_parser_execute` is a resumable, byte-at-a-time parser: feed it
whatever arrived and it fires callbacks at message boundaries. Five of them do
all the work:

* ``on_message_begin`` marks a request in flight;
* ``on_url`` accumulates the (possibly split) URL;
* ``on_headers_complete`` refuses upgrades with a 400 and opens the
  destination file for PUT/POST;
* ``on_body`` appends each body fragment to that file;
* ``on_message_complete`` answers directly from the server's internal staging
  buffer - safe even though the parser may keep reading the same packet, and
  what serves pipelined requests in order.

The answer streams the file back for GET, closes and returns 201 for uploads,
or emits the recorded error status.

There are no sockets and no state machine beyond the parser's own. The class
owns one thread, one packet queue and one file handle - and no mutex: the
server thread is the only thing touching request state, ``enqueue_packet()``
only touches the queue (a ``k_fifo``, safe from threads and ISRs), and the
single-consumer queue is what serialises requests. The sample adds one more
thread, the ``UartBridge`` feed thread, because allocating a packet may block
on an exhausted pool, which the UART ISR may not do.

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
flash simulator at ``/lfs`` and seeds a ``hello.txt`` into it. The board
overlay also grows ``storage_partition`` from its stock 16 KiB to 1 MiB -
littlefs metadata consumes most of 16 KiB, and uploads otherwise fail with
``-ENOSPC`` after a few hundred bytes. A real board would have mounted its
storage during boot and simply passed the path to the constructor.

At boot the sample injects four requests - a download, an upload, a read-back
and a 404 - so the mechanism is visible without a terminal attached:

.. code-block:: console

   <inf> uart_bridge: UART uart_1 wired to the HTTP server
   <inf> http_server_bare: --> request 1: feeding 46 bytes in two packets
   <inf> raw_http: GET /lfs/hello.txt
   <inf> http_server_bare: --> request 2: feeding 87 bytes in two packets
   <inf> raw_http: PUT /lfs/upload.txt
   <inf> http_server_bare: --> request 3: feeding 47 bytes in two packets
   <inf> raw_http: GET /lfs/upload.txt
   <inf> http_server_bare: --> request 4: feeding 48 bytes in two packets
   <inf> raw_http: GET /files/missing.txt -> -2
   <inf> http_server_bare: Self-test done, now serving the UART forever

Each request is handed over in two separate packets on purpose: the parser
consumes a byte stream, so requests may be split across as many packets as the
transport happens to produce. Set ``CONFIG_APP_SELFTEST=n`` to skip the
injected requests and serve only what arrives on the UART.

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

It downloads the seeded file, uploads a generated payload, reads it back and
compares byte for byte, checks that a missing file returns 404, and re-downloads
to prove the stream survived all of it. Exit status is 0 on pass, 1 on failure.
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
* ``Expect: 100-continue`` is not implemented. Clients that send it (curl does
  for large uploads) pause briefly before sending the body; pass
  ``-H 'Expect:'`` to avoid the delay.
* A request carrying ``Connection: close`` is served, but there is no
  connection to close - the stream simply keeps serving.
* ``UartBridge`` transmits with :c:func:`uart_poll_out`, which busy-waits per
  byte. That is fine at sane baud rates; switch to interrupt-driven TX if you
  push large responses at high speed.
* If the UART RX ring (``UART_BRIDGE_RING_SIZE``) overflows, bytes are
  dropped: the bridge logs the exact count and sends the server a stream-break
  marker (a zero-length packet). The server then fails any request in flight
  with a 400 and resets the parser, instead of silently sewing later bytes
  into a half-received upload. At any real baud rate the feed thread drains
  far faster than bytes arrive.
* An aborted upload (parser failure mid-body, stream break, or a write/close
  error) deletes the partial file - the old content was already truncated away
  at open, and an honest 404 beats serving a corrupt file with a 200. A failed
  ``fs_close()`` (e.g. ``-ENOSPC`` on the final flush) is answered 500, not
  201.
* On ``native_sim`` that overflow is easy to trip artificially: a
  pseudoterminal has no baud rate, and native_pty's interrupt-emulation thread
  runs at ``K_HIGHEST_THREAD_PRIO`` without sleeping. Pace host writes for
  large uploads - the test script does this by default, and ``--pace 0`` turns
  it off.
* HTTP has no framing below it, and there is no TCP providing reliability or
  ordering. Bytes must arrive in order, exactly once, with no gaps; a dropped
  or duplicated byte desynchronises the parser, which answers 400 and resets.
  Over a noisy UART, add framing or use a clean link.
