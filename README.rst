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

   static void to_my_link(const uint8_t *data, size_t len, void *user)
   {
           my_link_write(data, len);        /* bytes out of the server */
   }

   /* second argument is the already-mounted directory files live in */
   static RawHttpServer server(to_my_link, "/lfs");

   server.input(bytes_from_my_link, n);     /* bytes into the server */

There is nothing to start: everything happens synchronously inside ``input()``,
and by the time it returns the response has been handed to the output callback.
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

``RAW_HTTP_SERVER`` selects ``CPP``, ``FILE_SYSTEM``, ``HTTP_PARSER`` and
``NETWORKING``. The URL prefix, URL bound, download chunk size and log
level are Kconfig options under the ``RAW_HTTP_SERVER`` menu.

Files
*****

The constructor takes the path of a directory that is **already mounted** - the
server never mounts anything itself. Files are served under ``/files/``:

.. code-block:: console

   GET  /files/report.bin      download a file from <fs_root>/report.bin
   PUT  /files/report.bin      upload the request body to that path
   POST /files/report.bin      same as PUT

Downloads stream through a ``CONFIG_RAW_HTTP_FILE_CHUNK`` buffer (1600 bytes by default), so file
size is not bounded by RAM. The size comes from :c:func:`fs_stat`, so responses
carry a plain ``Content-Length`` - no chunked encoding. Uploads never touch
that buffer: body fragments are written to the file straight out of the
caller's input buffer as the parser delivers them, so they are not bounded
either. A missing file returns 404, a ``..`` in the name is rejected, and the
stream keeps serving.

No connection to lose
*********************

HTTP/1.1 keep-alive falls out of the design instead of being engineered in:
there is no connection object anywhere, just a resumable parser on a byte
stream. Requests may be split across any number of ``input()`` calls, and one
call may carry several requests. A malformed request is answered with a 400 and
the parser resets - nothing is lost beyond the malformed bytes themselves.

How it works
************

:c:func:`http_parser_execute` is a resumable, byte-at-a-time parser: feed it
whatever arrived and it fires callbacks at message boundaries. Four of them do
all the work:

* ``on_url`` accumulates the (possibly split) URL;
* ``on_headers_complete`` opens the destination file for PUT/POST;
* ``on_body`` appends each body fragment to that file;
* ``on_message_complete`` answers - it streams the file back for GET, closes
  and returns 201 for uploads, or emits the recorded error status.

There are no sockets, no threads and no state machine beyond the parser's own.
The class owns one mutex (so several threads may call ``input()``), one file
handle and one download buffer. The only thread in the sample is the
``UartBridge`` feed thread, because the UART ISR may not block and ``input()``
does file I/O.

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
   <inf> http_server_bare: --> request 1: feeding 46 bytes in two chunks
   <inf> raw_http: GET /lfs/hello.txt
   <inf> http_server_bare: --> request 2: feeding 87 bytes in two chunks
   <inf> raw_http: PUT /lfs/upload.txt
   <inf> http_server_bare: --> request 3: feeding 47 bytes in two chunks
   <inf> raw_http: GET /lfs/upload.txt
   <inf> http_server_bare: --> request 4: feeding 48 bytes in two chunks
   <inf> raw_http: GET /files/missing.txt -> -2
   <inf> http_server_bare: Self-test done, now serving the UART forever

Each request is handed over in two separate calls on purpose: the parser
consumes a byte stream, so requests may be split across as many buffers as the
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

* The output callback runs inside ``input()``, on the caller's thread. Never
  call ``input()`` from it.
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
* If the UART RX ring (``UART_BRIDGE_RING_SIZE``) overflows, bytes are dropped
  and the request stream desynchronises. The bridge logs an error when that
  happens. At any real baud rate the feed thread drains far faster than bytes
  arrive.
* On ``native_sim`` that overflow is easy to trip artificially: a
  pseudoterminal has no baud rate, and native_pty's interrupt-emulation thread
  runs at ``K_HIGHEST_THREAD_PRIO`` without sleeping. Pace host writes for
  large uploads - the test script does this by default, and ``--pace 0`` turns
  it off.
* HTTP has no framing below it, and there is no TCP providing reliability or
  ordering. Bytes must arrive in order, exactly once, with no gaps; a dropped
  or duplicated byte desynchronises the parser, which answers 400 and resets.
  Over a noisy UART, add framing or use a clean link.
