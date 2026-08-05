/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief A file server for HTTP/1.1 over bare data buffers.
 *
 * No sockets, no L2, no IP, no TCP - the only Zephyr networking code involved
 * is the HTTP request parser library. The whole surface is two operations:
 *
 *   - enqueue_packet(net_buf) - packets of raw request bytes go in
 *   - the OutputCallback given to the constructor - response bytes come out
 *
 * GET downloads a file from an already-mounted directory, PUT and POST upload
 * one into it. The server owns a thread that drains the packet queue; requests
 * are handled strictly one at a time, in arrival order - a packet enqueued
 * while a response is still streaming simply waits in the queue.
 *
 * The URL prefix, the URL/path bounds and the download chunk size are Kconfig
 * options: CONFIG_RAW_HTTP_FILES_PREFIX, CONFIG_RAW_HTTP_URL_MAX,
 * CONFIG_RAW_HTTP_PATH_MAX and CONFIG_RAW_HTTP_FILE_CHUNK. The thread is
 * shaped by CONFIG_RAW_HTTP_THREAD_STACK_SIZE and
 * CONFIG_RAW_HTTP_THREAD_PRIORITY.
 *
 * @code
 * NET_BUF_POOL_DEFINE(my_pool, 8, 1024, 0, NULL);
 *
 * static void to_my_link(struct net_buf *packet, size_t len, void *user)
 * {
 *         my_link_write(packet->data, len);
 * }
 *
 * static RawHttpServer server(to_my_link, "/lfs");
 *
 * struct net_buf *packet = net_buf_alloc(&my_pool, K_FOREVER);
 * net_buf_add_mem(packet, bytes_from_my_link, n);
 * server.enqueue_packet(packet);
 * @endcode
 */

#ifndef RAW_HTTP_SERVER_HPP_
#define RAW_HTTP_SERVER_HPP_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net_buf.h>

/**
 * @brief Serve files over HTTP/1.1, on buffers instead of a connection.
 *
 * The "connection" is whatever byte pipe the caller owns, so it never opens or
 * closes: every request is answered and the parser waits for the next one.
 * A malformed request gets a 400 and the parser resets, ready for the request
 * after it - nothing is ever lost beyond the malformed bytes themselves.
 *
 * All parsing, file I/O and response generation happen on the server's own
 * thread, which is the only thread touching any request state - so the class
 * needs no lock. enqueue_packet() only touches the packet queue and may be
 * called from any thread or from an ISR.
 *
 * The response is staged in the request's own packet: the net_buf that
 * completed a request is reused, cleared and refilled for every output
 * callback of that response, and released when the response is done. A fresh
 * packet is only ever taken from the queue for new request bytes.
 */
class RawHttpServer {
 public:
  /**
   * @brief Response bytes produced by the server.
   *
   * Invoked from the server's thread. Write the bytes onto your real
   * link - a UART, a USB endpoint, shared memory, a test harness.
   *
   * @note The payload is a byte stream: one response may arrive over
   *       several calls. Never assume a fixed size - @p len is whatever
   *       the server produced in one go.
   *
   * @note @p packet is the very net_buf the request arrived in, reused
   *       as the staging area for the whole response. The callback must
   *       copy or transmit the bytes before returning: the server clears
   *       and refills the buffer for the next piece of the response.
   *
   * @param packet Buffer holding the bytes to transmit. Never nullptr.
   * @param len Valid bytes in @p packet (equals packet->len). Never 0.
   * @param user_data The pointer handed to the constructor.
   */
  using OutputCallback = void (*)(struct net_buf* packet,
                                  size_t len,
                                  void* user_data);

  /**
   * @brief Longest response head the server emits, NUL included.
   *
   * Packet buffers double as the response staging area, so every buffer
   * handed to enqueue_packet() must have room for at least this many
   * bytes - size your net_buf pool accordingly.
   */
  static constexpr size_t response_head_max = 96;

  /**
   * @brief Construct the server and start its thread.
   *
   * @param output_callback Callback receiving response bytes. Must not be
   * nullptr.
   * @param fs_root Already-mounted directory that files are served from
   *                and uploaded into, e.g. "/lfs". Not created or mounted
   *                here.
   * @param user_data Opaque pointer forwarded to @p output_callback.
   */
  RawHttpServer(OutputCallback output_callback,
                const char* fs_root,
                void* user_data = nullptr);

  RawHttpServer(const RawHttpServer&) = delete;
  RawHttpServer& operator=(const RawHttpServer&) = delete;

  /**
   * @brief Hand a packet of raw request bytes to the server.
   *
   * The packet does not have to hold a whole request: the parser
   * consumes a byte stream, and a request may span any number of
   * packets. The server thread parses each packet in arrival order and
   * emits the response through the output callback before it takes the
   * next packet - requests are never handled in parallel.
   *
   * Ownership of @p packet passes to the server, which releases it back
   * to its pool when done. Bytes following a completed request inside
   * the same packet (a pipelined next request) are dropped with a
   * warning: the buffer is reused for the response. On this
   * request/response transport a client waits for the answer before
   * sending more, so nothing well-behaved ever hits this.
   *
   * Safe to call from any thread or from an ISR.
   *
   * @return 0 on success, -EINVAL on a nullptr packet.
   */
  int enqueue_packet(struct net_buf* packet);

  /** @brief The filesystem root passed to the constructor. */
  const char* fs_root() const { return _fs_root; }

 private:
  /**
   * @brief Parser callback: accumulate the (possibly split) URL.
   *
   * The parser hands the URL over in as many pieces as the transport
   * produced; this appends each piece to _url. Overflow marks the
   * request as failed with a 414 rather than truncating silently.
   */
  static int on_url(struct http_parser* parser, const char* at, size_t length);

  /**
   * @brief Parser callback: the request head is complete.
   *
   * For PUT and POST this opens the destination file, so that body
   * fragments can be written straight through as they arrive. Any other
   * method than GET/PUT/POST fails the request with a 405.
   */
  static int on_headers_complete(struct http_parser* parser);

  /**
   * @brief Parser callback: one fragment of the request body.
   *
   * Appends the fragment to the file opened by on_headers_complete().
   * A write failure closes the file and fails the request with a 500;
   * the rest of the body is then parsed but ignored.
   */
  static int on_body(struct http_parser* parser, const char* at, size_t length);

  /**
   * @brief Parser callback: the request is complete - pause the parser.
   *
   * The answer is deliberately not emitted here: the response is staged
   * in the very packet the parser is still reading, so it must not be
   * overwritten mid-parse. Pausing makes http_parser_execute() return
   * with HPE_PAUSED, and process_packet() answers once the packet's
   * bytes are no longer needed.
   */
  static int on_message_complete(struct http_parser* parser);

  /**
   * @brief Thread entry trampoline, matching k_thread_entry_t.
   */
  static void run_trampoline(void* p1, void* p2, void* p3);

  /**
   * @brief The server thread: take packets off the queue, forever.
   *
   * One packet at a time: parse it, answer any request it completed,
   * release it. Everything - parser callbacks, file I/O, the output
   * callback - runs here, so nothing else can interleave with a
   * request.
   */
  void run_loop();

  /**
   * @brief Parse one packet and answer any request it completes.
   *
   * Feeds the packet to the parser. If on_message_complete() paused the
   * parser, the request's bytes are consumed and the packet is free:
   * answer() reuses it for the response. Garbage gets a 400 and a
   * parser reset; a packet holding only a request fragment produces no
   * output at all.
   */
  void process_packet(struct net_buf* packet);

  /**
   * @brief Emit the response for the completed request.
   *
   * Answers with the recorded error, a streamed download for GET, or a
   * 201 for a finished upload. Afterwards the per-request state is
   * reset for the next request.
   */
  void answer();

  /**
   * @brief Map the request URL to an absolute path under the fs root.
   *
   * Strips the query string, requires CONFIG_RAW_HTTP_FILES_PREFIX, and
   * rejects ".." so nothing outside the root is reachable.
   *
   * @return 0 on success, -ENOENT for a URL outside the prefix or an
   *         empty/traversing name, -ENAMETOOLONG if it does not fit.
   */
  int build_path(char* out, size_t out_size) const;

  /**
   * @brief Serve a GET: open the file, send the head, stream the body.
   *
   * The size comes from fs_stat(), so the response carries a plain
   * Content-Length - no chunked encoding. A file that cannot be opened
   * or stat'd is a 404. Body chunks are read straight into the request's
   * packet, one output callback per chunk.
   */
  void send_file();

  /** @brief Stage a response head in the packet and emit it. */
  void respond(unsigned int status, size_t content_length);

  /** @brief Push the packet's current contents to the output callback. */
  void send();

  /** @brief Close any open file and clear the per-request state. */
  void reset_request();

  /** The parser callbacks, shared by every instance. */
  static const struct http_parser_settings _settings;

  OutputCallback _output_callback;
  void* _user_data;
  const char* _fs_root;

  struct http_parser _parser{};

  /** Packets from enqueue_packet(), consumed only by the server thread. */
  struct k_fifo _rx_fifo{};
  struct k_thread _thread{};
  K_KERNEL_STACK_MEMBER(_stack, CONFIG_RAW_HTTP_THREAD_STACK_SIZE);

  /** The packet being processed; the response is staged into it. */
  struct net_buf* _packet{nullptr};

  /* Per-request state, cleared by reset_request(). */
  char _url[CONFIG_RAW_HTTP_URL_MAX]{};
  size_t _url_len{0};
  struct fs_file_t _file{};
  bool _file_open{false};
  /** Nonzero once the request has failed; the status to answer with. */
  unsigned int _error_status{0};
};

#endif /* RAW_HTTP_SERVER_HPP_ */
