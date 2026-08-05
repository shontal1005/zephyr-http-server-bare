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
 *   - input(buffer, size) - raw request bytes go in
 *   - the OutputCallback given to the constructor - response bytes come out
 *
 * GET downloads a file from an already-mounted directory, PUT and POST upload
 * one into it. Everything runs synchronously inside input(): by the time it
 * returns, the response has been handed to the output callback.
 *
 * The URL prefix, the URL/path bounds and the download chunk size are Kconfig
 * options: CONFIG_RAW_HTTP_FILES_PREFIX, CONFIG_RAW_HTTP_URL_MAX,
 * CONFIG_RAW_HTTP_PATH_MAX and CONFIG_RAW_HTTP_FILE_CHUNK.
 *
 * @code
 * static void to_my_link(const uint8_t *data, size_t len, void *user)
 * {
 *         my_link_write(data, len);
 * }
 *
 * static RawHttpServer server(to_my_link, "/lfs");
 *
 * server.input(bytes_from_my_link, n);
 * @endcode
 */

#ifndef RAW_HTTP_SERVER_HPP_
#define RAW_HTTP_SERVER_HPP_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/parser.h>

/**
 * @brief Serve files over HTTP/1.1, on buffers instead of a connection.
 *
 * The "connection" is whatever byte pipe the caller owns, so it never opens or
 * closes: every request is answered and the parser waits for the next one.
 * A malformed request gets a 400 and the parser resets, ready for the request
 * after it - nothing is ever lost beyond the malformed bytes themselves.
 *
 * Thread-safe: input() serialises callers with a mutex. Single-client by
 * construction - one byte stream, one request at a time.
 */
class RawHttpServer {
public:
	/**
	 * @brief Response bytes produced by the server.
	 *
	 * Invoked from inside input(), on the caller's thread. Write the bytes
	 * onto your real link - a UART, a USB endpoint, shared memory, a test
	 * harness.
	 *
	 * @note The payload is a byte stream: one response may arrive over
	 *       several calls. Never assume a fixed size - @p len is whatever
	 *       the server produced in one go.
	 *
	 * @param data Bytes to transmit. Never nullptr.
	 * @param len Valid bytes in @p data. Never 0.
	 * @param user_data The pointer handed to the constructor.
	 */
	using OutputCallback = void (*)(const uint8_t *data, size_t len, void *user_data);

	/**
	 * @brief Construct the server. There is nothing else to start.
	 *
	 * @param cb Callback receiving response bytes. Must not be nullptr.
	 * @param fs_root Already-mounted directory that files are served from
	 *                and uploaded into, e.g. "/lfs". Not created or mounted
	 *                here.
	 * @param user_data Opaque pointer forwarded to @p cb.
	 */
	RawHttpServer(OutputCallback cb, const char *fs_root, void *user_data = nullptr);

	RawHttpServer(const RawHttpServer &) = delete;
	RawHttpServer &operator=(const RawHttpServer &) = delete;

	/**
	 * @brief Hand a buffer of raw request bytes to the server.
	 *
	 * The buffer does not have to hold a whole request and it may span
	 * several requests: the parser consumes a byte stream, not messages.
	 * Responses are emitted through the output callback before this
	 * returns, so the call blocks for the duration of any file I/O.
	 *
	 * A malformed request is answered with a 400 and the rest of the buffer
	 * is discarded; the next call starts a fresh request. Never call this
	 * from an ISR.
	 *
	 * @return 0 on success (including handled bad requests),
	 *         -EINVAL on a nullptr buffer.
	 */
	int input(const void *buffer, size_t size);

	/** @brief The filesystem root passed to the constructor. */
	const char *fs_root() const
	{
		return _fs_root;
	}

private:
	/**
	 * @brief Parser callback: accumulate the (possibly split) URL.
	 *
	 * The parser hands the URL over in as many pieces as the transport
	 * produced; this appends each piece to _url. Overflow marks the
	 * request as failed with a 414 rather than truncating silently.
	 */
	static int on_url(struct http_parser *parser, const char *at, size_t length);

	/**
	 * @brief Parser callback: the request head is complete.
	 *
	 * For PUT and POST this opens the destination file, so that body
	 * fragments can be written straight through as they arrive. Any other
	 * method than GET/PUT/POST fails the request with a 405.
	 */
	static int on_headers_complete(struct http_parser *parser);

	/**
	 * @brief Parser callback: one fragment of the request body.
	 *
	 * Appends the fragment to the file opened by on_headers_complete().
	 * A write failure closes the file and fails the request with a 500;
	 * the rest of the body is then parsed but ignored.
	 */
	static int on_body(struct http_parser *parser, const char *at, size_t length);

	/**
	 * @brief Parser callback: the request is complete - answer it.
	 *
	 * Emits the response for the collected request state: the recorded
	 * error, a streamed download for GET, or a 201 for a finished upload.
	 * Afterwards the per-request state is reset for the next request.
	 */
	static int on_message_complete(struct http_parser *parser);

	/**
	 * @brief Map the request URL to an absolute path under the fs root.
	 *
	 * Strips the query string, requires CONFIG_RAW_HTTP_FILES_PREFIX, and
	 * rejects ".." so nothing outside the root is reachable.
	 *
	 * @return 0 on success, -ENOENT for a URL outside the prefix or an
	 *         empty/traversing name, -ENAMETOOLONG if it does not fit.
	 */
	int build_path(char *out, size_t out_size) const;

	/**
	 * @brief Serve a GET: open the file, send the head, stream the body.
	 *
	 * The size comes from fs_stat(), so the response carries a plain
	 * Content-Length - no chunked encoding. A file that cannot be opened
	 * or stat'd is a 404.
	 */
	void send_file();

	/** @brief Emit a response head, and with it any zero-length response. */
	void respond(unsigned int status, size_t content_length);

	/** @brief Push bytes to the output callback. */
	void send(const void *data, size_t len);

	/** @brief Close any open file and clear the per-request state. */
	void reset_request();

	/** The parser callbacks, shared by every instance. */
	static const struct http_parser_settings _settings;

	OutputCallback _cb;
	void *_user_data;
	const char *_fs_root;

	struct http_parser _parser {};
	struct k_mutex _lock {};

	/* Per-request state, cleared by reset_request(). */
	char _url[CONFIG_RAW_HTTP_URL_MAX]{};
	size_t _url_len{0};
	struct fs_file_t _file {};
	bool _file_open{false};
	/** Nonzero once the request has failed; the status to answer with. */
	unsigned int _error_status{0};

	uint8_t _file_buf[CONFIG_RAW_HTTP_FILE_CHUNK]{};
};

#endif /* RAW_HTTP_SERVER_HPP_ */
