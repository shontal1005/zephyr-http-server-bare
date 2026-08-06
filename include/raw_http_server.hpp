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
 * The request URL is the filesystem path: "GET /lfs/report.bin" serves the
 * file at /lfs/report.bin through whatever is mounted there, and PUT/POST
 * upload to that path. The VFS is the validator - a URL outside every mount
 * simply fails the filesystem call and is answered with 404. The server owns
 * a thread that drains the packet queue; requests are handled strictly one at
 * a time, in arrival order - a packet enqueued while a response is still
 * streaming simply waits in the queue.
 *
 * The URL bound and the download chunk size are Kconfig options:
 * CONFIG_RAW_HTTP_URL_MAX and CONFIG_RAW_HTTP_FILE_CHUNK. The thread is
 * shaped by CONFIG_RAW_HTTP_THREAD_STACK_SIZE and
 * CONFIG_RAW_HTTP_THREAD_PRIORITY.
 *
 * @code
 * NET_BUF_POOL_DEFINE(my_pool, 8, 1024, 0, NULL);
 *
 * static void to_my_link(const uint8_t *data, size_t len, void *user)
 * {
 *         my_link_write(data, len);
 * }
 *
 * static RawHttpServer server(to_my_link);
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
 * Responses are staged in an internal buffer of CONFIG_RAW_HTTP_FILE_CHUNK
 * bytes, never in the request's packet - so a packet may carry several
 * pipelined requests, each answered in order as the parser reaches it.
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
   * @note The bytes are valid only during the call: copy or transmit
   *       them before returning.
   *
   * @param data Bytes to transmit. Never nullptr.
   * @param len Number of valid bytes at @p data. Never 0.
   * @param user_data The pointer handed to the constructor.
   */
  using OutputCallback = void (*)(const uint8_t* data,
                                  size_t len,
                                  void* user_data);

  /**
   * @brief Construct the server and start its thread.
   *
   * Files are served from whatever filesystems are mounted - the URL is
   * the filesystem path, so a mount at "/lfs" serves "GET /lfs/...".
   * Nothing is created or mounted here.
   *
   * @param output_callback Callback receiving response bytes. Must not be
   * nullptr.
   * @param user_data Opaque pointer forwarded to @p output_callback.
   */
  RawHttpServer(OutputCallback output_callback, void* user_data = nullptr);

  /**
   * @brief Stop the server thread and release everything it held.
   *
   * Best-effort: destroying mid-request simply drops that exchange.
   */
  ~RawHttpServer();

  RawHttpServer(const RawHttpServer&) = delete;
  RawHttpServer& operator=(const RawHttpServer&) = delete;

  /**
   * @brief Hand a packet of raw request bytes to the server.
   *
   * The parser consumes a byte stream: a request may span any number of
   * packets, and one packet may carry several pipelined requests - each
   * is answered in order. Ownership of @p packet passes to the server
   * (and stays with the caller on error). Flat buffers only - fragment
   * chains are rejected.
   *
   * A zero-length packet is the in-band stream-break marker: the
   * transport lost bytes, so any request in flight is failed with a
   * 400 and the parser resets.
   *
   * Safe to call from any thread or from an ISR.
   *
   * @return 0 on success, -EINVAL on a nullptr or chained packet.
   */
  int enqueue_packet(struct net_buf* packet);

 private:
  /**
   * @brief Parser callback: a new request has started.
   *
   * Marks a request in flight; reset_request() clears the mark, so the
   * pair keeps _request_active exact across any packet fragmentation.
   */
  static int on_message_begin(struct http_parser* parser);

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
   * An upgrade (CONNECT, Upgrade:) is refused with a 400 - no other
   * protocol is spoken here. The query string is cut off the URL: what
   * remains is the filesystem path. For PUT and POST this opens the
   * destination file, so that body fragments can be written straight
   * through as they arrive - a path the VFS refuses (no mount there,
   * ".." above a root, a directory) is a 404. Any other method than
   * GET/PUT/POST fails the request with a 405.
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
   * @brief Parser callback: the request is complete - answer it.
   *
   * The response is staged in _out_buf, not in the packet the parser
   * is reading, so answering inline is safe - and it is what serves
   * pipelined requests in order, one answer per completed message.
   */
  static int on_message_complete(struct http_parser* parser);

  /**
   * @brief Thread entry trampoline, matching k_thread_entry_t.
   */
  static void run_trampoline(void* p1, void* p2, void* p3);

  /**
   * @brief The server thread: take packets off the queue, forever.
   *
   * One packet at a time: parse it, answer any request it completed
   * inline via on_message_complete(), release it. An upgrade's
   * foreign-protocol remainder is dropped, garbage goes to
   * fail_stream(), a zero-length packet to handle_stream_break().
   * Everything - parser callbacks, file I/O, the output callback -
   * runs here, so nothing else can interleave with a request.
   */
  void run_loop();

  /**
   * @brief Answer 400 once per failure burst, then reset the parser.
   *
   * More 400s would sit stale in the client's buffer, misattributed
   * to its later, valid requests.
   */
  void fail_stream(const char* why);

  /**
   * @brief The transport lost bytes (a zero-length packet arrived).
   *
   * A request in flight can never complete: fail it so the client
   * gets closure. When idle, just reset the parser.
   */
  void handle_stream_break();

  /**
   * @brief Emit the response for the completed request.
   *
   * Answers with the recorded error, a streamed download for GET, or a
   * 201 for a finished upload. Afterwards the per-request state is
   * reset for the next request.
   */
  void answer();

  /**
   * @brief Serve a GET: open the file, send the head, stream the body.
   *
   * The URL is the path. The size comes from fs_stat(), so the response
   * carries a plain Content-Length - no chunked encoding. Anything that
   * cannot be stat'd or opened, or is not a regular file, is a 404.
   * Body chunks are read into _out_buf, one output callback per chunk.
   */
  void send_file();

  /** @brief Stage a response head in _out_buf and emit it. */
  void respond(unsigned int status, size_t content_length);

  /**
   * @brief Delete a failed upload's partial file.
   *
   * The old content is already truncated away; an honest 404 beats a
   * 200 serving a corrupt file.
   */
  void abort_upload();

  /** @brief Close any open file and clear the per-request state. */
  void reset_request();

  /**
   * @brief Fresh parser for the next request.
   *
   * Clears sticky parser flags (upgrade); the data pointer survives.
   */
  void reset_parser();

  /** The parser callbacks, shared by every instance. */
  static const struct http_parser_settings _settings;

  OutputCallback _output_callback;
  void* _user_data;

  struct http_parser _parser{};

  /** Packets from enqueue_packet(), consumed only by the server thread. */
  struct k_fifo _rx_fifo{};
  struct k_thread _thread{};
  K_KERNEL_STACK_MEMBER(_stack, CONFIG_RAW_HTTP_THREAD_STACK_SIZE);

  /** Response staging: heads and download chunks alike. */
  uint8_t _out_buf[CONFIG_RAW_HTTP_FILE_CHUNK]{};

  /** A 400 for the current failure burst was already sent. */
  bool _bad_stream{false};

  /* Per-request state, cleared by reset_request(). */
  char _url[CONFIG_RAW_HTTP_URL_MAX]{};
  size_t _url_len{0};
  struct fs_file_t _file{};
  bool _file_open{false};
  /** A request is mid-parse: on_message_begin() sets, reset_request()
   * clears. */
  bool _request_active{false};
  /** An upload at _url is open but not yet committed. abort_upload()
   * unlinks _url, so reset_request() clears _url only after it ran. */
  bool _upload_pending{false};
  /** Nonzero once the request has failed; the status to answer with. */
  unsigned int _error_status{0};
};

#endif /* RAW_HTTP_SERVER_HPP_ */
