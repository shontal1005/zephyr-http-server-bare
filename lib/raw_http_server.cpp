/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net/http/status.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "raw_http_server.hpp"

LOG_MODULE_REGISTER(raw_http, CONFIG_RAW_HTTP_LOG_LEVEL);

namespace {

// Not an HTTP limit - the protocol has none. Worst case of the head format in
// respond(): "HTTP/1.1 500 Internal Server Error" + CRLF + "Content-Length: "
// with a 20-digit 64-bit size + closing CRLFs = 76 chars, 77 with the NULL.
constexpr size_t response_head_max = 96;

BUILD_ASSERT(CONFIG_RAW_HTTP_FILE_CHUNK >= (int)response_head_max,
             "The staging buffer must be able to hold a response head");

// The handful of statuses this server actually emits.
const char* reason_of(unsigned int status) {
  switch (status) {
    case HTTP_200_OK:
      return "OK";
    case HTTP_400_BAD_REQUEST:
      return "Bad Request";
    case HTTP_404_NOT_FOUND:
      return "Not Found";
    case HTTP_405_METHOD_NOT_ALLOWED:
      return "Method Not Allowed";
    case HTTP_414_URI_TOO_LONG:
      return "URI Too Long";
    case HTTP_431_REQUEST_HEADER_FIELDS_TOO_LARGE:
      return "Request Header Fields Too Large";
    case HTTP_500_INTERNAL_SERVER_ERROR:
    default:
      // Also the safety net for a status this switch does not know.
      return "Internal Server Error";
  }
}

} /* namespace */

const struct http_parser_settings RawHttpServer::_settings = {
    .on_url = &RawHttpServer::on_url,
    .on_headers_complete = &RawHttpServer::on_headers_complete,
};

RawHttpServer::RawHttpServer(OutputCallback output_callback, void* user_data)
    : _output_callback(output_callback), _user_data(user_data) {
  k_fifo_init(&_rx_fifo);

  // process_request() reinitialises the parser per parse attempt and
  // http_parser_init() preserves this pointer, so set it once here.
  _parser.data = this;

  k_thread_create(&_thread, _stack, K_KERNEL_STACK_SIZEOF(_stack),
                  run_trampoline, this, nullptr, nullptr,
                  CONFIG_RAW_HTTP_THREAD_PRIORITY, 0, K_NO_WAIT);
}

RawHttpServer::~RawHttpServer() {
  // Stop the thread first, then release everything it may have held.
  k_thread_abort(&_thread);

  struct net_buf* packet;
  while ((packet = static_cast<struct net_buf*>(
              k_fifo_get(&_rx_fifo, K_NO_WAIT))) != nullptr) {
    net_buf_unref(packet);
  }
}

int RawHttpServer::enqueue_packet(struct net_buf* packet) {
  // A chain would be misparsed: only the head fragment is ever read.
  if (packet == nullptr || packet->frags != nullptr) {
    return -EINVAL;
  }

  k_fifo_put(&_rx_fifo, packet);
  return 0;
}

void RawHttpServer::run_trampoline(void* p1, void* p2, void* p3) {
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  static_cast<RawHttpServer*>(p1)->run_loop();
}

void RawHttpServer::run_loop() {
  while (true) {
    struct net_buf* packet =
        static_cast<struct net_buf*>(k_fifo_get(&_rx_fifo, K_FOREVER));

    // A zero-length packet carries no bytes: nothing to do.
    if (packet->len > 0) {
      handle_packet(packet);
    }

    net_buf_unref(packet);
  }
}

size_t RawHttpServer::discard(size_t available) {
  // MIN in the debt's 64-bit width (it may exceed SIZE_MAX); the result
  // is bounded by available, so the cast back to size_t is safe.
  size_t paid = (size_t)MIN((uint64_t)available, _body_to_discard);

  _body_to_discard -= paid;
  return paid;
}

void RawHttpServer::handle_packet(const struct net_buf* packet) {
  const uint8_t* data = packet->data;
  size_t len = packet->len;

  // Bytes owed to an answered request's body are not ours to parse:
  // they pay the debt off the packet front, never buffered.
  size_t skipped = discard(len);

  data += skipped;
  len -= skipped;

  if (len == 0) {
    return;
  }

  size_t take = MIN(sizeof(_request_buf) - _request_len, len);

  memcpy(_request_buf + _request_len, data, take);
  _request_len += take;

  bool answered = process_request();

  // No head and no room left: the head outgrew the buffer. Answer and
  // reset - the stream self-heals at the next parseable request.
  if (!answered && _request_len == sizeof(_request_buf)) {
    LOG_WRN("Request head longer than %zu, refused", sizeof(_request_buf));
    respond(HTTP_431_REQUEST_HEADER_FIELDS_TOO_LARGE, 0);
    _request_len = 0;
  }

  // One request per round trip: the rest of the packet is dropped. If
  // the head completed exactly as the buffer filled, this remainder is
  // the answered request's own body, so it too pays the debt.
  discard(len - take);
}

bool RawHttpServer::process_request() {
  // A fresh parse from the buffer start each time: no parser state
  // survives between attempts, so packet boundaries cannot matter.
  http_parser_init(&_parser, HTTP_REQUEST);
  _url[0] = '\0';

  size_t parsed = http_parser_execute(
      &_parser, &_settings, reinterpret_cast<const char*>(_request_buf),
      _request_len);

  // on_headers_complete() halts the parser on purpose, so the errno
  // is the whole verdict - and HPE_OK honestly means "incomplete".
  enum http_errno err = HTTP_PARSER_ERRNO(&_parser);

  if (err == HPE_OK) {
    // The head is not all here yet: wait for more bytes.
    return false;
  }

  if (err == HPE_CB_url) {
    // on_url() fails for exactly one reason: the URL outgrew _url.
    LOG_WRN("URL longer than %zu, refused", sizeof(_url) - 1);
    respond(HTTP_414_URI_TOO_LONG, 0);
    _request_len = 0;
    return true;
  }

  if (err != HPE_CB_headers_complete) {
    // Malformed bytes; boundaries are unknown, so drop the lot.
    LOG_WRN("Unparseable bytes dropped (%s)", http_errno_name(err));
    respond(HTTP_400_BAD_REQUEST, 0);
    _request_len = 0;
    return true;
  }

  // One complete head. The parse stopped at the final LF, which is
  // not yet counted - hence the +1.
  size_t head_len = parsed + 1;

  if (_parser.upgrade) {
    // No other protocol is spoken here (CONNECT included), and what
    // follows an upgrade head is not HTTP: drop the lot.
    LOG_WRN("Upgrade request refused");
    respond(HTTP_400_BAD_REQUEST, 0);
    _request_len = 0;
    return true;
  }

  if ((_parser.flags & F_CHUNKED) != 0) {
    // A chunked body has no predeclared length to skip past.
    LOG_WRN("Chunked request refused");
    respond(HTTP_400_BAD_REQUEST, 0);
    _request_len = 0;
    return true;
  }

  if (_parser.method != HTTP_GET) {
    LOG_WRN("%s refused: GET is the whole surface",
            http_method_str((enum http_method)_parser.method));
    respond(HTTP_405_METHOD_NOT_ALLOWED, 0);
  } else {
    // Cut the query string off: what remains is the filesystem
    // path, handed to the VFS as-is - the mount table does the
    // validation that a path builder otherwise would.
    char* query = strchr(_url, '?');
    if (query != nullptr) {
      *query = '\0';
    }

    send_file();
  }

  // Answered: the full body length becomes the discard debt (no
  // Content-Length header parses as the all-ones sentinel, the parser's
  // ULLONG_MAX = no body). The bytes already buffered past the head are
  // dropped with the reset below, so they pay the debt first; the rest
  // is discarded as it streams in.
  _body_to_discard =
      _parser.content_length == UINT64_MAX ? 0 : _parser.content_length;
  discard(_request_len - head_len);
  _request_len = 0;

  return true;
}

int RawHttpServer::on_url(struct http_parser* parser,
                          const char* at,
                          size_t length) {
  RawHttpServer* self = static_cast<RawHttpServer*>(parser->data);

  // The vendored parser fires this at most once per execute on a
  // freshly initialised parse (one URL mark, flushed at end of data),
  // so the span always starts at the URL's first byte.
  //
  // Refuse an oversized URL rather than truncating, which would
  // silently address the wrong file. Failing the parse here surfaces
  // as HPE_CB_url, which process_request() answers with 414.
  if (length >= sizeof(self->_url)) {
    return -1;
  }

  memcpy(self->_url, at, length);
  self->_url[length] = '\0';

  return 0;
}

int RawHttpServer::on_headers_complete(struct http_parser* parser) {
  ARG_UNUSED(parser);

  // Halt the parser exactly here: the head is everything a GET-only
  // server needs, and the body - if any - is skipped by length, never
  // parsed. The resulting HPE_CB_headers_complete errno is
  // process_request()'s success verdict. (0, 1 and 2 are magic values
  // to this callback; -1 is not.)
  return -1;
}

void RawHttpServer::send_file() {
  struct fs_dirent entry;
  struct fs_file_t file;

  // Stat before open: the size must be on the wire as Content-Length
  // before the first body byte. The VFS resolves the URL through its
  // mount table - a path outside every mount just fails here.
  int ret = fs_stat(_url, &entry);

  // A directory (a mount point included) is not downloadable.
  if (ret == 0 && entry.type != FS_DIR_ENTRY_FILE) {
    ret = -EISDIR;
  }

  if (ret == 0) {
    fs_file_t_init(&file);
    ret = fs_open(&file, _url, FS_O_READ);
  }

  if (ret < 0) {
    LOG_INF("GET %s -> %d", _url, ret);
    respond(HTTP_404_NOT_FOUND, 0);
    return;
  }

  LOG_INF("GET %s", _url);
  respond(HTTP_200_OK, entry.size);

  for (;;) {
    ssize_t got = fs_read(&file, _out_buf, sizeof(_out_buf));

    if (got <= 0) {
      // A read error cannot be signalled any more - the head
      // already promised entry.size bytes. Stop and log.
      if (got < 0) {
        LOG_ERR("fs_read failed (%d)", (int)got);
      }
      break;
    }

    _output_callback(_out_buf, (size_t)got, _user_data);
  }

  (void)fs_close(&file);
}

void RawHttpServer::respond(unsigned int status, size_t content_length) {
  // The BUILD_ASSERT above guarantees _out_buf holds any head.
  int len = snprintk(reinterpret_cast<char*>(_out_buf), sizeof(_out_buf),
                     "HTTP/1.1 %u %s\r\nContent-Length: %zu\r\n\r\n", status,
                     reason_of(status), content_length);

  _output_callback(_out_buf, (size_t)len, _user_data);
}
