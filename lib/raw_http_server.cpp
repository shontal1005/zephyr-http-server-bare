/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net/http/status.h>
#include <zephyr/sys/printk.h>

#include "raw_http_server.hpp"

LOG_MODULE_REGISTER(raw_http, CONFIG_RAW_HTTP_LOG_LEVEL);

namespace {

// response_head_max lives in the header: it is part of the API contract now,
// because callers must size their net_buf pool to hold a staged head.
// Its derivation: worst case of the head format in respond() is
// "HTTP/1.1 500 Internal Server Error" + CRLF + "Content-Length: " with a
// 20-digit 64-bit size + closing CRLFs = 76 chars, 77 with the NUL.

// The handful of statuses this server actually emits.
const char* reason_of(unsigned int status) {
  switch (status) {
    case HTTP_200_OK:
      return "OK";
    case HTTP_201_CREATED:
      return "Created";
    case HTTP_400_BAD_REQUEST:
      return "Bad Request";
    case HTTP_404_NOT_FOUND:
      return "Not Found";
    case HTTP_405_METHOD_NOT_ALLOWED:
      return "Method Not Allowed";
    case HTTP_414_URI_TOO_LONG:
      return "URI Too Long";
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
    .on_body = &RawHttpServer::on_body,
    .on_message_complete = &RawHttpServer::on_message_complete,
};

RawHttpServer::RawHttpServer(OutputCallback output_callback,
                             const char* fs_root,
                             void* user_data)
    : _output_callback(output_callback),
      _user_data(user_data),
      _fs_root(fs_root) {
  k_fifo_init(&_rx_fifo);

  // HTTP_REQUEST is server mode: the parser rolls over to the next
  // request by itself, which is all the keep-alive handling there is.
  http_parser_init(&_parser, HTTP_REQUEST);
  _parser.data = this;

  k_thread_create(&_thread, _stack, K_KERNEL_STACK_SIZEOF(_stack),
                  run_trampoline, this, nullptr, nullptr,
                  CONFIG_RAW_HTTP_THREAD_PRIORITY, 0, K_NO_WAIT);
  (void)k_thread_name_set(&_thread, "raw_http");
}

int RawHttpServer::enqueue_packet(struct net_buf* packet) {
  if (packet == nullptr || _output_callback == nullptr) {
    return -EINVAL;
  }

  // A net_buf's first word is reserved for exactly this. No lock: the
  // fifo is safe against concurrent producers, threads and ISRs alike,
  // and the single consumer thread is what serialises the requests.
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

    // The packet is the response staging area too, so it is held
    // for as long as the response it may trigger is being sent;
    // packets queued meanwhile simply wait their turn.
    _packet = packet;
    process_packet(packet);
    _packet = nullptr;

    net_buf_unref(packet);
  }
}

void RawHttpServer::process_packet(struct net_buf* packet) {
  // The on_*() callbacks do all the work; the parser keeps its own
  // position, so the packet may hold a whole request or any fragment of
  // one without any handling here.
  size_t parsed = http_parser_execute(
      &_parser, &_settings, reinterpret_cast<const char*>(packet->data),
      packet->len);
  enum http_errno err = HTTP_PARSER_ERRNO(&_parser);

  // on_message_complete() paused the parser: a request is complete and
  // its bytes are consumed, so the packet is free to carry the response.
  if (err == HPE_PAUSED) {
    http_parser_pause(&_parser, 0);

    // Anything after the completed request would be a pipelined
    // next request; the response is about to overwrite it. On this
    // request/response transport a client waits for the answer
    // before sending more, so only misbehaviour ends up here.
    if (parsed < packet->len) {
      LOG_WRN("Dropping %zu pipelined bytes after a complete request",
              packet->len - parsed);
    }

    answer();
    return;
  }

  // Garbage or an upgrade request (not spoken here): answer 400, drop
  // the rest of the packet and reset so the next packet starts fresh.
  if (parsed != packet->len || _parser.upgrade || err != HPE_OK) {
    LOG_WRN("Malformed request dropped (%s)",
            http_errno_name(HTTP_PARSER_ERRNO(&_parser)));

    respond(HTTP_400_BAD_REQUEST, 0);
    reset_request();
    http_parser_init(&_parser, HTTP_REQUEST);
  }

  // Otherwise the packet held a request fragment: nothing to say yet.
}

void RawHttpServer::answer() {
  if (_error_status != 0) {
    // The request failed somewhere along the way.
    respond(_error_status, 0);
  } else if (_parser.method == HTTP_GET) {
    // A download: open the file and stream it back.
    send_file();
  } else if (_parser.method == HTTP_PUT || _parser.method == HTTP_POST) {
    // An upload: the whole body is on disk, close and confirm.
    (void)fs_close(&_file);
    _file_open = false;
    respond(HTTP_201_CREATED, 0);
  } else {
    // Unreachable: on_headers_complete() already failed every
    // other method with 405. Answer something sane just in case.
    respond(HTTP_500_INTERNAL_SERVER_ERROR, 0);
  }

  reset_request();
}

int RawHttpServer::on_url(struct http_parser* parser,
                          const char* at,
                          size_t length) {
  RawHttpServer* self = static_cast<RawHttpServer*>(parser->data);

  // The URL arrives in fragments. Poison an oversized one rather than
  // truncating, which would silently address the wrong file.
  if (self->_url_len + length >= sizeof(self->_url)) {
    self->_error_status = HTTP_414_URI_TOO_LONG;
    return 0;
  }

  memcpy(self->_url + self->_url_len, at, length);
  self->_url_len += length;
  self->_url[self->_url_len] = '\0';

  return 0;
}

int RawHttpServer::on_headers_complete(struct http_parser* parser) {
  RawHttpServer* self = static_cast<RawHttpServer*>(parser->data);

  if (self->_error_status != 0) {
    return 0;
  }

  switch (parser->method) {
    case HTTP_GET:
      // No body; everything happens once the message completes.
      return 0;

    case HTTP_PUT:
    case HTTP_POST: {
      // The destination must be open before the first body fragment:
      // fragments are written straight through, never buffered.
      char path[CONFIG_RAW_HTTP_PATH_MAX];
      int ret = self->build_path(path, sizeof(path));
      if (ret < 0) {
        self->_error_status = HTTP_404_NOT_FOUND;
        return 0;
      }

      fs_file_t_init(&self->_file);
      // Open the file with truncate, writing from start.
      ret = fs_open(&self->_file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
      if (ret < 0) {
        LOG_ERR("Cannot open %s for upload (%d)", path, ret);
        self->_error_status = HTTP_500_INTERNAL_SERVER_ERROR;
        return 0;
      }

      LOG_INF("%s %s", http_method_str((enum http_method)parser->method), path);
      self->_file_open = true;
      return 0;
    }

    default:
      self->_error_status = HTTP_405_METHOD_NOT_ALLOWED;
      return 0;
  }
}

int RawHttpServer::on_body(struct http_parser* parser,
                           const char* at,
                           size_t length) {
  RawHttpServer* self = static_cast<RawHttpServer*>(parser->data);

  // Already failed: the body still streams through, but goes nowhere.
  if (!self->_file_open) {
    return 0;
  }

  ssize_t written = fs_write(&self->_file, at, length);

  if (written < 0 || (size_t)written != length) {
    // Keep parsing so the stream stays in sync and the client gets
    // a clean 500 at the end of the request.
    LOG_ERR("fs_write failed (%d)", (int)written);
    (void)fs_close(&self->_file);
    self->_file_open = false;
    self->_error_status = HTTP_500_INTERNAL_SERVER_ERROR;
  }

  return 0;
}

int RawHttpServer::on_message_complete(struct http_parser* parser) {
  // Do not answer from here: the response would be staged in the very
  // packet the parser is still reading. Pause instead - execute()
  // returns HPE_PAUSED and process_packet() answers with the packet
  // free for reuse.
  http_parser_pause(parser, 1);

  return 0;
}

int RawHttpServer::build_path(char* out, size_t out_size) const {
  const size_t prefix_len = sizeof(CONFIG_RAW_HTTP_FILES_PREFIX) - 1;

  if (strncmp(_url, CONFIG_RAW_HTTP_FILES_PREFIX, prefix_len) != 0) {
    return -ENOENT;
  }

  const char* name = _url + prefix_len;

  // A query string is not part of the filename.
  const char* query = strchr(name, '?');
  size_t name_len = (query != nullptr) ? (size_t)(query - name) : strlen(name);

  if (name_len == 0) {
    return -ENOENT;
  }

  // Nothing above the root is reachable.
  if (strstr(name, "..") != nullptr) {
    return -ENOENT;
  }

  if (snprintk(out, out_size, "%s/%.*s", _fs_root, (int)name_len, name) >=
      (int)out_size) {
    return -ENAMETOOLONG;
  }

  return 0;
}

void RawHttpServer::send_file() {
  char path[CONFIG_RAW_HTTP_PATH_MAX];
  struct fs_dirent entry;
  int ret = build_path(path, sizeof(path));

  // Stat before open: the size must be on the wire as Content-Length
  // before the first body byte.
  if (ret == 0) {
    ret = fs_stat(path, &entry);
  }

  if (ret == 0) {
    fs_file_t_init(&_file);
    ret = fs_open(&_file, path, FS_O_READ);
  }

  if (ret < 0) {
    LOG_INF("GET %s -> %d", _url, ret);
    respond(HTTP_404_NOT_FOUND, 0);
    return;
  }

  LOG_INF("GET %s", path);
  _file_open = true;
  respond(HTTP_200_OK, entry.size);

  // Body chunks are staged in the request's packet, bounded by both the
  // buffer's capacity and the Kconfig chunk knob.
  size_t chunk = MIN(_packet->size, (size_t)CONFIG_RAW_HTTP_FILE_CHUNK);

  for (;;) {
    net_buf_reset(_packet);
    ssize_t got = fs_read(&_file, _packet->data, chunk);

    if (got <= 0) {
      // A read error cannot be signalled any more - the head
      // already promised entry.size bytes. Stop and log.
      if (got < 0) {
        LOG_ERR("fs_read failed (%d)", (int)got);
      }
      break;
    }

    net_buf_add(_packet, (size_t)got);
    send();
  }

  (void)fs_close(&_file);
  _file_open = false;
}

void RawHttpServer::respond(unsigned int status, size_t content_length) {
  net_buf_reset(_packet);

  // The head is written straight into the packet; response_head_max is
  // the documented lower bound on the pool's buffer size.
  int len = snprintk(reinterpret_cast<char*>(_packet->data), _packet->size,
                     "HTTP/1.1 %u %s\r\nContent-Length: %zu\r\n\r\n", status,
                     reason_of(status), content_length);

  net_buf_add(_packet, (size_t)len);
  send();
}

void RawHttpServer::send() {
  _output_callback(_packet, _packet->len, _user_data);
}

void RawHttpServer::reset_request() {
  // Only an abandoned request still holds a file here.
  if (_file_open) {
    (void)fs_close(&_file);
    _file_open = false;
  }

  _url_len = 0;
  _url[0] = '\0';
  _error_status = 0;
}
