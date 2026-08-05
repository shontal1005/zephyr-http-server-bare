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

namespace
{

// Not an HTTP limit - the protocol has none. Worst case of the head format in
// respond(): "HTTP/1.1 500 Internal Server Error" + CRLF + "Content-Length: "
// with a 20-digit 64-bit size + closing CRLFs = 76 chars, 77 with the NUL.
constexpr size_t response_head_max = 96;

// The handful of statuses this server actually emits.
const char *reason_of(unsigned int status)
{
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

RawHttpServer::RawHttpServer(OutputCallback cb, const char *fs_root, void *user_data)
	: _cb(cb), _user_data(user_data), _fs_root(fs_root)
{
	k_mutex_init(&_lock);

	// HTTP_REQUEST is server mode: the parser rolls over to the next
	// request by itself, which is all the keep-alive handling there is.
	http_parser_init(&_parser, HTTP_REQUEST);
	_parser.data = this;
}

int RawHttpServer::input(const void *buffer, size_t size)
{
	if (buffer == nullptr || _cb == nullptr) {
		return -EINVAL;
	}

	k_mutex_lock(&_lock, K_FOREVER);

	// The on_*() callbacks below do all the work; the parser keeps its own
	// position, so the buffer may hold request fragments or several whole
	// requests without any handling here.
	size_t parsed = http_parser_execute(&_parser, &_settings,
					    static_cast<const char *>(buffer), size);

	// Garbage or an upgrade request (not spoken here): answer 400, drop
	// the rest of the buffer and reset so the next call starts fresh.
	if (parsed != size || _parser.upgrade || HTTP_PARSER_ERRNO(&_parser) != HPE_OK) {
		LOG_WRN("Malformed request dropped (%s)",
			http_errno_name(HTTP_PARSER_ERRNO(&_parser)));

		respond(HTTP_400_BAD_REQUEST, 0);
		reset_request();
		http_parser_init(&_parser, HTTP_REQUEST);
	}

	k_mutex_unlock(&_lock);

	return 0;
}

int RawHttpServer::on_url(struct http_parser *parser, const char *at, size_t length)
{
	RawHttpServer *self = static_cast<RawHttpServer *>(parser->data);

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

int RawHttpServer::on_headers_complete(struct http_parser *parser)
{
	RawHttpServer *self = static_cast<RawHttpServer *>(parser->data);

	if (self->_error_status != 0) {
		return 0;
	}

	switch (parser->method) {
	case HTTP_GET:
		// No body; everything happens in on_message_complete().
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
		// FS_O_TRUNC: a shorter re-upload must not keep the old tail.
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

int RawHttpServer::on_body(struct http_parser *parser, const char *at, size_t length)
{
	RawHttpServer *self = static_cast<RawHttpServer *>(parser->data);

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

int RawHttpServer::on_message_complete(struct http_parser *parser)
{
	RawHttpServer *self = static_cast<RawHttpServer *>(parser->data);

	if (self->_error_status != 0) {
		// The request failed somewhere along the way.
		self->respond(self->_error_status, 0);
	} else if (parser->method == HTTP_GET) {
		// A download: open the file and stream it back.
		self->send_file();
	} else if (parser->method == HTTP_PUT || parser->method == HTTP_POST) {
		// An upload: the whole body is on disk, close and confirm.
		(void)fs_close(&self->_file);
		self->_file_open = false;
		self->respond(HTTP_201_CREATED, 0);
	} else {
		// Unreachable: on_headers_complete() already failed every
		// other method with 405. Answer something sane just in case.
		self->respond(HTTP_500_INTERNAL_SERVER_ERROR, 0);
	}

	self->reset_request();

	return 0;
}

int RawHttpServer::build_path(char *out, size_t out_size) const
{
	const size_t prefix_len = sizeof(CONFIG_RAW_HTTP_FILES_PREFIX) - 1;

	if (strncmp(_url, CONFIG_RAW_HTTP_FILES_PREFIX, prefix_len) != 0) {
		return -ENOENT;
	}

	const char *name = _url + prefix_len;

	// A query string is not part of the filename.
	const char *query = strchr(name, '?');
	size_t name_len = (query != nullptr) ? (size_t)(query - name) : strlen(name);

	if (name_len == 0) {
		return -ENOENT;
	}

	// Nothing above the root is reachable.
	if (strstr(name, "..") != nullptr) {
		return -ENOENT;
	}

	if (snprintk(out, out_size, "%s/%.*s", _fs_root, (int)name_len, name) >= (int)out_size) {
		return -ENAMETOOLONG;
	}

	return 0;
}

void RawHttpServer::send_file()
{
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
		ssize_t got = fs_read(&_file, _file_buf, sizeof(_file_buf));

		if (got <= 0) {
			// A read error cannot be signalled any more - the head
			// already promised entry.size bytes. Stop and log.
			if (got < 0) {
				LOG_ERR("fs_read failed (%d)", (int)got);
			}
			break;
		}

		send(_file_buf, (size_t)got);
	}

	(void)fs_close(&_file);
	_file_open = false;
}

void RawHttpServer::respond(unsigned int status, size_t content_length)
{
	char head[response_head_max];
	int len = snprintk(head, sizeof(head), "HTTP/1.1 %u %s\r\nContent-Length: %zu\r\n\r\n",
			   status, reason_of(status), content_length);

	send(head, (size_t)len);
}

void RawHttpServer::send(const void *data, size_t len)
{
	_cb(static_cast<const uint8_t *>(data), len, _user_data);
}

void RawHttpServer::reset_request()
{
	// Only an abandoned request still holds a file here.
	if (_file_open) {
		(void)fs_close(&_file);
		_file_open = false;
	}

	_url_len = 0;
	_url[0] = '\0';
	_error_status = 0;
}
