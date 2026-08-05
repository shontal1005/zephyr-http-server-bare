/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Raw (network-less) front end for the Zephyr HTTP server.
 *
 * The HTTP server always talks to its peers through a socket, but it never
 * cares what is behind that socket. RawHttpServer exploits that: it gives the
 * stock, unmodified server a socket that is not a network socket, and exposes
 * the whole thing as two plain operations:
 *
 *   - input(buffer, size) - raw bytes go into the server
 *   - the OutputCallback given to the constructor - raw bytes come back out
 *
 * No L2 driver, no IP and no TCP is involved at any point.
 *
 * @code
 * static void toMyLink(const uint8_t *data, size_t len, void *user)
 * {
 *         my_link_write(data, len);
 * }
 *
 * static RawHttpServer server(toMyLink);
 *
 * server.start();
 * server.input(bytes_from_my_link, n);
 * @endcode
 */

#ifndef RAW_HTTP_HPP_
#define RAW_HTTP_HPP_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>

/** Bytes passed to the output callback at most in one go. */
#define RAW_HTTP_RX_BUF_SIZE 1600

/** URL prefix under which files are served. Register the resource with it. */
#define RAW_HTTP_FILES_PREFIX "/files/"

/**
 * Bytes read from the filesystem per download callback.
 *
 * Pure throughput-vs-RAM tradeoff, and it costs exactly this many bytes inside
 * every RawHttpServer instance. Nothing downstream constrains it: the chunked
 * encoding re-states the length for each chunk, and a chunk larger than
 * CONFIG_NET_SOCKETPAIR_BUFFER_SIZE simply makes the server's blocking send()
 * drain in more than one go. Uploads do not use this buffer at all - they are
 * written straight from the request body, whose size the client buffer governs.
 */
#define RAW_HTTP_FILE_CHUNK 1600

/** Longest absolute path this server will build. */
#define RAW_HTTP_PATH_MAX 128

/**
 * @brief The Zephyr HTTP server, driven by bare data buffers.
 *
 * The connection is persistent. An HTTP/1.1 request that does not carry
 * "Connection: close" leaves it open, so one connection serves requests
 * indefinitely. If the server does hang up - on a malformed request, say -
 * input() re-establishes it on a following call; see the note on input() for
 * what that costs.
 *
 * @note One instance at a time. The listening socket is a process-wide
 *       singleton, so a second concurrent start() fails with -EEXIST. This is
 *       a single-client design by construction.
 */
class RawHttpServer {
public:
	/**
	 * @brief Raw bytes produced by the HTTP server.
	 *
	 * Registered through the constructor. Write them onto your real link -
	 * a UART, a USB endpoint, shared memory, a test harness.
	 *
	 * @note Invoked from the RX thread, never from the HTTP server thread.
	 *       The payload is a byte stream, so one HTTP response may arrive
	 *       over several calls, and one call may carry the tail of one
	 *       response and the head of the next. Never assume a fixed size:
	 *       @p len is whatever happened to be available.
	 *
	 * @param data Bytes from the server, or nullptr when the server hung up.
	 * @param len Valid bytes in @p data, 0 when the server hung up.
	 * @param user_data The pointer handed to the constructor.
	 */
	using OutputCallback = void (*)(const uint8_t *data, size_t len, void *user_data);

	/**
	 * @brief Construct the front end.
	 *
	 * Does no work beyond storing its arguments, so an instance is safe at
	 * namespace scope. Call start() to bring it up.
	 *
	 * @param cb Callback receiving the server's output. Must not be nullptr.
	 * @param user_data Opaque pointer forwarded to @p cb.
	 */
	/**
	 * @param cb Callback receiving the server's output. Must not be nullptr.
	 * @param fs_root Already-mounted directory that files are served from and
	 *                uploaded into, e.g. "/lfs". Not created or mounted here.
	 * @param user_data Opaque pointer forwarded to @p cb.
	 */
	explicit RawHttpServer(OutputCallback cb, const char *fs_root, void *user_data = nullptr)
		: cb_(cb), userData_(user_data), fsRoot_(fs_root)
	{
	}

	~RawHttpServer();

	RawHttpServer(const RawHttpServer &) = delete;
	RawHttpServer &operator=(const RawHttpServer &) = delete;

	/**
	 * @brief Start the HTTP server and establish the connection.
	 *
	 * @return 0 on success, -EEXIST if an instance is already running,
	 *         another negative errno otherwise.
	 */
	int start();

	/**
	 * @brief Hand a buffer of raw request bytes to the HTTP server.
	 *
	 * The buffer does not have to hold a whole request, and it may span
	 * several requests: the server parses a byte stream, not messages.
	 *
	 * @note Recovery is not seamless. The server closes on a malformed
	 *       request, and the buffer that races that close is lost - measured
	 *       behaviour is that one request goes missing before the relink
	 *       takes effect; the one after it succeeds. A buffer that is only
	 *       partially delivered is dropped rather than straddling two
	 *       connections, which would arrive as garbage on both.
	 *
	 * Blocks until every byte has been handed over, which happens when the
	 * server is slower than the caller. Never call it from the output
	 * callback, and never from an ISR.
	 *
	 * @return 0 on success, negative errno otherwise.
	 */
	int input(const void *buffer, size_t size);

	/** @brief Whether the connection to the server is currently up. */
	bool isConnected() const
	{
		return linked_;
	}

	/** @brief Tear the connection down. start() may be called again after. */
	void stop();

	/**
	 * @brief Listening socket factory, for http_service_config::socket_create.
	 *
	 * Pass this to the HTTP_SERVICE_DEFINE() that declares your resources:
	 *
	 * @code
	 * static const struct http_service_config cfg = {
	 *         .socket_create = RawHttpServer::socketCreate,
	 * };
	 * @endcode
	 */
	static int socketCreate(const struct http_service_desc *svc, int af, int proto);

	/**
	 * @brief Resource callback serving files out of, and into, the fs root.
	 *
	 * GET streams a file back; PUT and POST write the request body to one.
	 * Register it on RAW_HTTP_FILES_PREFIX "*" with this instance as the
	 * resource's user_data:
	 *
	 * @code
	 * static struct http_resource_detail_dynamic files = {
	 *         .common = { .bitmask_of_supported_http_methods =
	 *                             BIT(HTTP_GET) | BIT(HTTP_PUT) | BIT(HTTP_POST),
	 *                     .type = HTTP_RESOURCE_TYPE_DYNAMIC, ... },
	 *         .cb = RawHttpServer::fileHandler,
	 * };
	 * files.user_data = &server;      // at runtime, before start()
	 * @endcode
	 *
	 * @note Responses are chunked: a dynamic resource has no Content-Length.
	 * @note One transfer at a time, which the server's per-resource holder
	 *       already enforces for a single client.
	 */
	static int fileHandler(struct http_client_ctx *client, enum http_transaction_status status,
			       const struct http_request_ctx *request_ctx,
			       struct http_response_ctx *response_ctx, void *user_data);

	/** @brief The filesystem root passed to the constructor. */
	const char *fsRoot() const
	{
		return fsRoot_;
	}

private:
	int link();
	static void rxTrampoline(void *p1, void *p2, void *p3);
	void rxLoop();

	int handleFile(struct http_client_ctx *client, enum http_transaction_status status,
		       const struct http_request_ctx *request_ctx,
		       struct http_response_ctx *response_ctx);
	int handleDownload(struct http_client_ctx *client, struct http_response_ctx *response_ctx);
	int handleUpload(struct http_client_ctx *client, enum http_transaction_status status,
			 const struct http_request_ctx *request_ctx,
			 struct http_response_ctx *response_ctx);
	int buildPath(const char *url, char *out, size_t out_size);
	void closeTransfer();

	OutputCallback cb_;
	void *userData_;
	const char *fsRoot_;

	/* In-flight file transfer. Single-client by design, so one is enough.
	 * Separate from rxBuf_ on purpose: the RX thread may be draining a
	 * response into the output callback while the server thread fills this.
	 */
	struct fs_file_t file_ {};
	bool fileOpen_{false};
	uint8_t fileBuf_[RAW_HTTP_FILE_CHUNK]{};
	int appFd_{-1};
	struct k_mutex lock_ {};
	struct k_thread thread_ {};
	bool started_{false};
	volatile bool linked_{false};
	bool threadStarted_{false};
	volatile bool running_{false};
	uint8_t rxBuf_[RAW_HTTP_RX_BUF_SIZE]{};
};

#endif /* RAW_HTTP_HPP_ */
