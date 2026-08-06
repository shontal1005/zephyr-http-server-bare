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

// Not an HTTP limit - the protocol has none. Worst case of the head format in
// respond(): "HTTP/1.1 500 Internal Server Error" + CRLF + "Content-Length: "
// with a 20-digit 64-bit size + closing CRLFs = 76 chars, 77 with the NUL.
constexpr size_t response_head_max = 96;

BUILD_ASSERT(CONFIG_RAW_HTTP_FILE_CHUNK >= (int)response_head_max,
             "The staging buffer must be able to hold a response head");

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
    .on_message_begin = &RawHttpServer::on_message_begin,
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
}

RawHttpServer::~RawHttpServer() {
  // Stop the thread first, then release everything it may have held.
  k_thread_abort(&_thread);

  struct net_buf* packet;
  while ((packet = static_cast<struct net_buf*>(
              k_fifo_get(&_rx_fifo, K_NO_WAIT))) != nullptr) {
    net_buf_unref(packet);
  }

  abort_upload();
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

    process_packet(packet);

    net_buf_unref(packet);
  }
}

void RawHttpServer::process_packet(struct net_buf* packet) {
  // A zero-length packet is the transport saying it lost bytes.
  if (packet->len == 0) {
    handle_stream_break();
    return;
  }

  // The parser keeps its own position: any request fragmentation is fine.
  // Every request the packet completes is answered inline from
  // on_message_complete(), pipelined ones included.
  size_t parsed = http_parser_execute(
      &_parser, &_settings, reinterpret_cast<const char*>(packet->data),
      packet->len);
  enum http_errno err = HTTP_PARSER_ERRNO(&_parser);

  // An upgrade was already answered inline - on_headers_complete()
  // poisons every upgrade with a 400. The parser stopped consuming, so
  // the remainder is upgraded-protocol bytes, not a stream failure.
  if (_parser.upgrade) {
    if (parsed < packet->len) {
      LOG_WRN("Dropping %zu upgraded-protocol bytes", packet->len - parsed);
    }

    reset_parser();
    return;
  }

  // Garbage: fail the stream (a single 400, see fail_stream()). With no
  // pause and no upgrade, the parser consumes every byte or sets errno.
  if (err != HPE_OK) {
    fail_stream(http_errno_name(err));
    return;
  }

  // A non-keep-alive request parks a strict-mode parser dead - there is
  // no connection to close, so start fresh instead. Only between
  // messages: the parser clears its flags at the next request's first
  // byte (which also fires on_message_begin), so !_request_active means
  // the flags this check reads still describe the finished request.
  if (!_request_active && http_should_keep_alive(&_parser) == 0) {
    reset_parser();
  }
}

void RawHttpServer::fail_stream(const char* why) {
  // One 400 per burst: more would be misattributed to later requests.
  if (!_bad_stream) {
    LOG_WRN("Malformed request dropped (%s)", why);
    respond(HTTP_400_BAD_REQUEST, 0);
    _bad_stream = true;
  } else {
    LOG_DBG("Still in a failed stream (%s), dropped silently", why);
  }

  reset_request();
  reset_parser();
}

void RawHttpServer::handle_stream_break() {
  // _request_active is authoritative: every parse path either sets it or
  // resets all per-request state.
  if (_request_active) {
    // The request in flight can never complete - give the client closure.
    fail_stream("bytes lost by the transport");
  } else {
    LOG_WRN("Stream break while idle");
    reset_parser();
  }
}

void RawHttpServer::answer() {
  if (_error_status != 0) {
    // The request failed somewhere along the way.
    respond(_error_status, 0);
  } else if (_parser.method == HTTP_GET) {
    // A download: open the file and stream it back.
    send_file();
  } else if (_parser.method == HTTP_PUT || _parser.method == HTTP_POST) {
    // An upload is only durable once the close's cache flush succeeds;
    // a failed flush means a truncated file - delete it and answer 500.
    int ret = fs_close(&_file);
    _file_open = false;

    if (ret < 0) {
      LOG_ERR("fs_close failed (%d)", ret);
      abort_upload();
      respond(HTTP_500_INTERNAL_SERVER_ERROR, 0);
    } else {
      _upload_path[0] = '\0';
      respond(HTTP_201_CREATED, 0);
    }
  } else {
    // Unreachable: on_headers_complete() already failed every
    // other method with 405. Answer something sane just in case.
    respond(HTTP_500_INTERNAL_SERVER_ERROR, 0);
  }

  reset_request();
}

int RawHttpServer::on_message_begin(struct http_parser* parser) {
  RawHttpServer* self = static_cast<RawHttpServer*>(parser->data);

  self->_request_active = true;

  return 0;
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

  // The parser sets the flag before invoking this callback. No other
  // protocol is spoken here, so an upgrade is refused outright.
  if (parser->upgrade) {
    LOG_WRN("Upgrade request refused");
    self->_error_status = HTTP_400_BAD_REQUEST;
    return 0;
  }

  if (self->_error_status != 0) {
    return 0;
  }

  switch (parser->method) {
    case HTTP_GET:
      // No body; everything happens once the message completes.
      return 0;

    case HTTP_PUT:
    case HTTP_POST: {
      // The destination must be open before the first body fragment.
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
      // Set only on success: non-empty means an uncommitted file exists.
      strcpy(self->_upload_path, path);
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
    // Keep parsing for stream sync; the client gets a clean 500 at the
    // end and the partial file goes with the failure.
    LOG_ERR("fs_write failed (%d)", (int)written);
    self->abort_upload();
    self->_error_status = HTTP_500_INTERNAL_SERVER_ERROR;
  }

  return 0;
}

int RawHttpServer::on_message_complete(struct http_parser* parser) {
  RawHttpServer* self = static_cast<RawHttpServer*>(parser->data);

  // A request that parses to completion ends any failure burst.
  self->_bad_stream = false;

  // The response stages in _out_buf, so answering here is safe even
  // though the parser may keep reading this packet afterwards.
  self->answer();

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

  for (;;) {
    ssize_t got = fs_read(&_file, _out_buf, sizeof(_out_buf));

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

  (void)fs_close(&_file);
  _file_open = false;
}

void RawHttpServer::respond(unsigned int status, size_t content_length) {
  // The BUILD_ASSERT above guarantees _out_buf holds any head.
  int len = snprintk(reinterpret_cast<char*>(_out_buf), sizeof(_out_buf),
                     "HTTP/1.1 %u %s\r\nContent-Length: %zu\r\n\r\n", status,
                     reason_of(status), content_length);

  _output_callback(_out_buf, (size_t)len, _user_data);
}

void RawHttpServer::abort_upload() {
  if (_file_open) {
    (void)fs_close(&_file);
    _file_open = false;
  }

  // The old content is already truncated away: an honest 404 later
  // beats a confident 200 serving a corrupt partial file.
  if (_upload_path[0] != '\0') {
    LOG_WRN("Upload aborted, deleting partial %s", _upload_path);
    (void)fs_unlink(_upload_path);
    _upload_path[0] = '\0';
  }
}

void RawHttpServer::reset_request() {
  // Only an abandoned upload still holds a file or a path here.
  abort_upload();

  _url_len = 0;
  _url[0] = '\0';
  _error_status = 0;
  _request_active = false;
}

void RawHttpServer::reset_parser() {
  // Also clears sticky flags (upgrade); the data pointer is preserved.
  http_parser_init(&_parser, HTTP_REQUEST);
}
