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
#include <zephyr/sys/printk.h>

#include "raw_http_server.hpp"

LOG_MODULE_REGISTER(raw_http, CONFIG_RAW_HTTP_LOG_LEVEL);

namespace
{

/* The handful of statuses this server actually emits. */
const char *reason_of(unsigned int status)
{
	switch (status) {
	case 200:
		return "OK";
	case 201:
		return "Created";
	case 400:
		return "Bad Request";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 414:
		return "URI Too Long";
	default:
		return "Internal Server Error";
	}
}

} /* namespace */

/* Only four of the parser's ten callbacks are needed: everything between them
 * (status line, header fields, chunk framing) is handled inside the parser
 * itself and never has to surface here.
 */
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

	/* HTTP_REQUEST puts the parser in server mode: it expects request
	 * lines, and rolls over to the next request by itself after each
	 * message - that is all the keep-alive handling there is to do.
	 */
	http_parser_init(&_parser, HTTP_REQUEST);

	/* The parser carries this pointer into every static callback, which is
	 * how they find their way back to the instance.
	 */
	_parser.data = this;
}

int RawHttpServer::input(const void *buffer, size_t size)
{
	if (buffer == nullptr || _cb == nullptr) {
		return -EINVAL;
	}

	k_mutex_lock(&_lock, K_FOREVER);

	/* Everything happens inside this call: the parser walks the buffer and
	 * fires the on_*() callbacks below, which do the file I/O and hand the
	 * response to the output callback. The buffer may hold a fragment of a
	 * request or several whole ones - the parser keeps its own position
	 * across calls, so neither case needs any handling here.
	 */
	size_t parsed = http_parser_execute(&_parser, &_settings,
					    static_cast<const char *>(buffer), size);

	/* parsed < size means the parser stopped mid-buffer on garbage. An
	 * upgrade request (e.g. Websocket) stops it without an error; this
	 * server does not speak anything but plain HTTP/1.1, so both cases get
	 * the same treatment: answer 400, drop the rest of the buffer, and
	 * re-init the parser so the next buffer starts a fresh request.
	 */
	if (parsed != size || _parser.upgrade || HTTP_PARSER_ERRNO(&_parser) != HPE_OK) {
		LOG_WRN("Malformed request dropped (%s)",
			http_errno_name(HTTP_PARSER_ERRNO(&_parser)));

		respond(400, 0);
		reset_request();
		http_parser_init(&_parser, HTTP_REQUEST);
	}

	k_mutex_unlock(&_lock);

	return 0;
}

int RawHttpServer::on_url(struct http_parser *parser, const char *at, size_t length)
{
	RawHttpServer *self = static_cast<RawHttpServer *>(parser->data);

	/* The URL arrives in as many fragments as input() buffers happened to
	 * split it into, so it has to be accumulated - it cannot be used until
	 * the head is complete anyway.
	 */
	if (self->_url_len + length >= sizeof(self->_url)) {
		/* Poison the request rather than truncating: a truncated name
		 * would silently address the wrong file.
		 */
		self->_error_status = 414;
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

	/* The URL was too long; there is nothing sane to open. The body, if
	 * any, still has to be parsed out of the stream, so no early answer.
	 */
	if (self->_error_status != 0) {
		return 0;
	}

	switch (parser->method) {
	case HTTP_GET:
		/* Downloads carry no body; everything happens in
		 * on_message_complete(). Opening the file this early would
		 * only mean holding it open for no reason.
		 */
		return 0;

	case HTTP_PUT:
	case HTTP_POST: {
		/* Uploads are the opposite: the destination must be open
		 * before the first body fragment arrives, because fragments
		 * are written straight to the file - the class never buffers
		 * the body, which is what makes upload size unbounded.
		 */
		char path[CONFIG_RAW_HTTP_PATH_MAX];
		int ret = self->build_path(path, sizeof(path));

		if (ret < 0) {
			self->_error_status = 404;
			return 0;
		}

		fs_file_t_init(&self->_file);
		ret = fs_open(&self->_file, path, FS_O_CREATE | FS_O_WRITE);

		if (ret == 0) {
			/* FS_O_CREATE does not truncate an existing file, and
			 * a shorter re-upload must not leave the old tail
			 * behind.
			 */
			ret = fs_truncate(&self->_file, 0);
			if (ret < 0) {
				(void)fs_close(&self->_file);
			}
		}

		if (ret < 0) {
			LOG_ERR("Cannot open %s for upload (%d)", path, ret);
			self->_error_status = 500;
			return 0;
		}

		LOG_INF("%s %s", http_method_str((enum http_method)parser->method), path);
		self->_file_open = true;
		return 0;
	}

	default:
		/* DELETE, HEAD and friends are not served. The 405 is emitted
		 * in on_message_complete(), like every other answer.
		 */
		self->_error_status = 405;
		return 0;
	}
}

int RawHttpServer::on_body(struct http_parser *parser, const char *at, size_t length)
{
	RawHttpServer *self = static_cast<RawHttpServer *>(parser->data);

	/* No file means the request already failed. The remaining fragments
	 * still stream through here - they have to be parsed to find the end
	 * of the message - but they go nowhere.
	 */
	if (!self->_file_open) {
		return 0;
	}

	/* @p at points into the caller's input() buffer: the body goes from
	 * the wire to the filesystem with no copy in between.
	 */
	ssize_t written = fs_write(&self->_file, at, length);

	if (written < 0 || (size_t)written != length) {
		/* Typically -ENOSPC. Give up on the file but keep parsing, so
		 * the stream stays in sync and the client gets a clean 500
		 * instead of a desynchronised parser.
		 */
		LOG_ERR("fs_write failed (%d)", (int)written);
		(void)fs_close(&self->_file);
		self->_file_open = false;
		self->_error_status = 500;
	}

	return 0;
}

int RawHttpServer::on_message_complete(struct http_parser *parser)
{
	RawHttpServer *self = static_cast<RawHttpServer *>(parser->data);

	/* The whole request has been parsed - this is the one place answers
	 * are sent from, so every request produces exactly one response, in
	 * order, even when several arrived in a single input() buffer.
	 */
	if (self->_error_status != 0) {
		self->respond(self->_error_status, 0);
	} else if (parser->method == HTTP_GET) {
		self->send_file();
	} else {
		/* A PUT or POST whose body is fully on disk. */
		(void)fs_close(&self->_file);
		self->_file_open = false;
		self->respond(201, 0);
	}

	/* Clear the per-request state; the parser rolls over to the next
	 * request in the stream by itself.
	 */
	self->reset_request();

	return 0;
}

int RawHttpServer::build_path(char *out, size_t out_size) const
{
	const size_t prefix_len = sizeof(CONFIG_RAW_HTTP_FILES_PREFIX) - 1;

	/* Anything outside /files/ simply does not exist here. */
	if (strncmp(_url, CONFIG_RAW_HTTP_FILES_PREFIX, prefix_len) != 0) {
		return -ENOENT;
	}

	const char *name = _url + prefix_len;

	/* A query string is not part of the filename. */
	const char *query = strchr(name, '?');
	size_t name_len = (query != nullptr) ? (size_t)(query - name) : strlen(name);

	if (name_len == 0) {
		return -ENOENT;
	}

	/* Nothing above the root is reachable. */
	if (strstr(name, "..") != nullptr) {
		return -ENOENT;
	}

	/* snprintk returns what it would have written, so >= out_size means
	 * the path got cut - reject it rather than touching the wrong file.
	 */
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

	/* Stat before open: the size goes into the Content-Length header,
	 * which must be on the wire before the first body byte.
	 */
	if (ret == 0) {
		ret = fs_stat(path, &entry);
	}

	if (ret == 0) {
		fs_file_t_init(&_file);
		ret = fs_open(&_file, path, FS_O_READ);
	}

	/* Whatever failed - bad prefix, no such file, unreadable - the client
	 * only needs to know the file is not there.
	 */
	if (ret < 0) {
		LOG_INF("GET %s -> %d", _url, ret);
		respond(404, 0);
		return;
	}

	LOG_INF("GET %s", path);
	_file_open = true;
	respond(200, entry.size);

	/* Stream the body one chunk at a time, so file size is bounded by the
	 * filesystem and not by RAM. Each chunk goes straight to the output
	 * callback; the caller's link provides any pacing needed.
	 */
	for (;;) {
		ssize_t got = fs_read(&_file, _file_buf, sizeof(_file_buf));

		if (got <= 0) {
			/* A read error mid-stream cannot be signalled any
			 * more - the head already promised entry.size bytes.
			 * All there is to do is stop and log.
			 */
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
	/* The head is complete as-is: HTTP/1.1 defaults to keep-alive, and
	 * Content-Length - zero included - tells the client exactly where
	 * this response ends and the next may begin.
	 */
	char head[96];
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
	/* Only an abandoned request still holds a file here - the normal
	 * paths close it themselves. Closing it drops any half-written
	 * upload's buffered tail.
	 */
	if (_file_open) {
		(void)fs_close(&_file);
		_file_open = false;
	}

	_url_len = 0;
	_url[0] = '\0';
	_error_status = 0;
}
