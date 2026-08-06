/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief A GET-only file server for HTTP/1.1 over bare data buffers.
 *
 * No sockets, no L2, no IP, no TCP - the only Zephyr networking code involved
 * is the HTTP request parser library. The whole surface is two operations:
 *
 *   - enqueue_packet(net_buf) - chunks of the request byte stream go in
 *   - the OutputCallback given to the constructor - response bytes come out
 *
 * The request URL is the filesystem path: "GET /lfs/report.bin" serves the
 * file at /lfs/report.bin through whatever is mounted there. The VFS is the
 * validator - a URL outside every mount simply fails the filesystem call and
 * is answered with 404. The server owns a thread that drains the packet
 * queue; requests are handled strictly one at a time, in arrival order.
 *
 * Packets carry arbitrary chunks of the byte stream - the server does the
 * framing. Request heads are assembled in an internal buffer, so a request
 * may be split across any number of packets. The server answers one request
 * per client round trip: once a request is answered, everything already
 * received beyond it is dropped, so a client must read the response before
 * sending its next request. A refused request's body is never buffered:
 * it is counted down and discarded as it streams past.
 *
 * The head-assembly bound and the download chunk size are Kconfig options:
 * CONFIG_RAW_HTTP_HEAD_MAX and CONFIG_RAW_HTTP_FILE_CHUNK, plus
 * CONFIG_RAW_HTTP_URL_MAX for the URL alone. The thread is shaped by
 * CONFIG_RAW_HTTP_THREAD_STACK_SIZE and CONFIG_RAW_HTTP_THREAD_PRIORITY.
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

#include <zephyr/kernel.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net_buf.h>

/**
 * @brief Serve file downloads over HTTP/1.1, on a raw byte stream.
 *
 * The "connection" is whatever byte pipe the caller owns, so it never opens
 * or closes. Every request gets exactly one response: 200 with the file, or
 * 404 (nothing servable at that path), 405 (not a GET), 414 (URL too long),
 * 431 (head outgrew the assembly buffer), 400 (malformed, chunked, or an
 * upgrade). An incomplete request is simply waited on - the head completes
 * whenever its bytes arrive. A request sent ahead of the previous response
 * is dropped, never answered - its tail may later parse as garbage and
 * earn a stray 400, from which the stream self-heals. Unparseable bytes
 * cost a single 400 and a stream reset; the stream self-heals at the next
 * request boundary.
 *
 * All parsing, file I/O and response generation happen on the server's own
 * thread, which is the only thread touching any request state - so the class
 * needs no lock. enqueue_packet() only touches the packet queue and may be
 * called from any thread or from an ISR.
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
   * @brief Hand a chunk of the request byte stream to the server.
   *
   * Packets are arbitrary chunks: a request may span any number of
   * packets - the server reassembles it. One request is answered per
   * round trip: bytes received beyond the answered request are dropped,
   * so do not send a request before reading the previous response. A
   * zero-length packet carries no bytes and is dropped silently.
   *
   * Ownership of @p packet passes to the server (and stays with the
   * caller on error). Flat buffers only - fragment chains are rejected.
   *
   * Safe to call from any thread or from an ISR.
   *
   * @return 0 on success, -EINVAL on a nullptr or chained packet.
   */
  int enqueue_packet(struct net_buf* packet);

 private:
  /**
   * @brief Parser callback: capture the URL.
   *
   * Called at most once per parse attempt - a URL split across packets
   * is re-seen whole here on the next attempt, because every attempt
   * re-parses from the buffer start. Fails the parse on overflow -
   * process_request() reads that back as HPE_CB_url and answers 414
   * rather than truncating silently.
   */
  static int on_url(struct http_parser* parser, const char* at, size_t length);

  /**
   * @brief Parser callback: the head is complete - halt the parser.
   *
   * Always returns nonzero, on purpose: the head is everything a
   * GET-only server needs, so the body is never parsed - it is skipped
   * by Content-Length in process_request(). The resulting
   * HPE_CB_headers_complete errno is process_request()'s success
   * verdict.
   */
  static int on_headers_complete(struct http_parser* parser);

  /**
   * @brief Thread entry trampoline, matching k_thread_entry_t.
   */
  static void run_trampoline(void* p1, void* p2, void* p3);

  /**
   * @brief The server thread: take packets off the queue, forever.
   *
   * Every non-empty packet goes to handle_packet(). Everything - the
   * parser, file I/O, the output callback - runs here, so nothing else
   * can interleave with a request.
   */
  void run_loop();

  /**
   * @brief Feed one packet of stream bytes through the server.
   *
   * Two consumers take the packet in order: discard() strips body
   * bytes owed to the previously answered request off the front, then
   * what fits of the rest is appended to _request_buf and
   * process_request() makes one parse attempt. A head that outgrows a
   * full buffer is answered with 431 and the buffer reset. Once a
   * request is answered the packet's remainder is dropped - through
   * discard() again, since it may be that request's own body - so at
   * most one response leaves per packet.
   */
  void handle_packet(const struct net_buf* packet);

  /**
   * @brief Pay down the answered request's body debt from stream bytes.
   *
   * Consumes up to @p available bytes against _body_to_discard. This is
   * the only place the debt is ever decremented, so every discard site
   * reads identically - and the size_t/uint64_t care lives here alone
   * (the debt can exceed SIZE_MAX on 32-bit targets).
   *
   * @param available Contiguous stream bytes on offer.
   * @return How many of them the debt consumed (at most @p available).
   */
  size_t discard(size_t available);

  /**
   * @brief One parse attempt over _request_buf; answer if it completes.
   *
   * Reinitialises the parser and parses the buffer from the start, so
   * no parser state survives between packets.
   * on_headers_complete() halts the parser at the end of the head,
   * which makes the parser's errno the whole verdict:
   * HPE_CB_headers_complete is success, HPE_OK means the head is
   * still incomplete (wait for more bytes), HPE_CB_url is an overlong
   * URL (414), anything else is malformed bytes (400). On success the
   * request is answered (upgrade/CONNECT 400, chunked 400, non-GET
   * 405, GET served), the full Content-Length is armed as
   * _body_to_discard, the bytes already buffered past the head pay it
   * down through discard() - they are dropped with the buffer reset -
   * and the body remainder still in flight is discarded on arrival.
   *
   * @return true if a response was sent, false if the head is still
   *         incomplete.
   */
  bool process_request();

  /**
   * @brief Serve the GET: open the file, send the head, stream the body.
   *
   * The URL is the path. The size comes from fs_stat(), so the response
   * carries a plain Content-Length - no chunked encoding. Anything that
   * cannot be stat'd or opened, or is not a regular file, is a 404.
   * Body chunks are read into _out_buf, one output callback per chunk.
   */
  void send_file();

  /** @brief Stage a response head in _out_buf and emit it. */
  void respond(unsigned int status, size_t content_length);

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

  /** Request heads are assembled here from the incoming packets. */
  uint8_t _request_buf[CONFIG_RAW_HTTP_HEAD_MAX]{};
  size_t _request_len{0};

  /** Body bytes still owed to an answered request, discarded on arrival. */
  uint64_t _body_to_discard{0};

  /* Per-parse state, reset by process_request() before each attempt. */
  char _url[CONFIG_RAW_HTTP_URL_MAX]{};
};

#endif /* RAW_HTTP_SERVER_HPP_ */
