/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Driving the Zephyr HTTP server with bare data buffers.
 *
 * The HTTP server runs completely unmodified, but there is no network stack
 * underneath it: no L2 driver, no IP, no TCP. A UART is wired straight into it,
 * so bytes arriving on the wire become HTTP requests and responses go back out
 * of the same UART.
 *
 * The connection is persistent: HTTP/1.1 keep-alive means one connection serves
 * requests indefinitely, and the transport relinks by itself if the server ever
 * does hang up.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>

#include "raw_http.hpp"
#include "uart_bridge.hpp"

LOG_MODULE_REGISTER(net_http_server_bare, LOG_LEVEL_INF);

/*
 * The HTTP server builds a sockaddr before it calls the socket_create hook, so
 * a service still needs a host and a port even though neither is used. Keep the
 * port non-zero: a zero port means "ephemeral", which would make the server
 * call getsockname() on the bare listening socket.
 */
static uint16_t bare_port = 80;

static const struct http_service_config bare_config = {
	.socket_create = RawHttpServer::socketCreate,
};

HTTP_SERVICE_DEFINE(bare_service, nullptr, &bare_port, 1, 1, nullptr, nullptr, &bare_config);

/* A static resource, served straight out of flash. */
static const char index_html[] =
	"<html><body><h1>Zephyr HTTP server, no network attached</h1></body></html>";

static struct http_resource_detail_static index_html_resource_detail = {
	.common = {
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.path_len = 0,
		.content_encoding = nullptr,
		.content_type = "text/html",
	},
	.static_data = index_html,
	.static_data_len = sizeof(index_html) - 1,
};

HTTP_RESOURCE_DEFINE(index_html_resource, bare_service, "/", &index_html_resource_detail);

/* A dynamic resource, to show that application handlers work as usual. */
static int uptime_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx, void *user_data)
{
	static uint8_t body[48];

	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	response_ctx->body_len = snprintf(reinterpret_cast<char *>(body), sizeof(body),
					  "{\"uptime_ms\":%lld}\n", k_uptime_get());
	response_ctx->body = body;
	response_ctx->final_chunk = true;

	return 0;
}

static struct http_resource_detail_dynamic uptime_resource_detail = {
	.common = {
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.path_len = 0,
		.content_encoding = nullptr,
		.content_type = "application/json",
	},
	.cb = uptime_handler,
	.holder = nullptr,
	.user_data = nullptr,
};

HTTP_RESOURCE_DEFINE(uptime_resource, bare_service, "/uptime", &uptime_resource_detail);

/* The UART carrying HTTP. Constructing it does no work, so namespace scope is
 * safe; everything happens in start().
 */
static UartBridge bridge(DEVICE_DT_GET(DT_ALIAS(http_uart)));

#if defined(CONFIG_APP_SELFTEST)
/*
 * Inject a few requests on the UART's own connection, so keep-alive can be
 * demonstrated without a terminal attached. Note the absence of
 * "Connection: close": that is the whole point, the connection stays up.
 */
static const char *const selftest_requests[] = {
	"GET / HTTP/1.1\r\nHost: bare\r\n\r\n",
	"GET /uptime HTTP/1.1\r\nHost: bare\r\n\r\n",
	"GET /uptime HTTP/1.1\r\nHost: bare\r\n\r\n",
};

static void run_selftest(RawHttpServer &server)
{
	ARRAY_FOR_EACH(selftest_requests, i) {
		const char *request = selftest_requests[i];
		size_t len = strlen(request);
		size_t first = len / 2;
		int ret;

		LOG_INF("--> request %zu: feeding %zu bytes in two chunks", i + 1, len);

		/* Split on purpose: the server does not care about buffer
		 * boundaries, only about the byte stream.
		 */
		ret = server.input(request, first);
		if (ret == 0) {
			ret = server.input(request + first, len - first);
		}

		if (ret < 0) {
			LOG_ERR("input failed (%d)", ret);
			return;
		}

		k_sleep(K_MSEC(200));

		LOG_INF("    connection still up: %s", server.isConnected() ? "yes" : "no");
	}
}
#endif /* CONFIG_APP_SELFTEST */

int main(void)
{
	int ret;

	ret = bridge.start();
	if (ret < 0) {
		LOG_ERR("Cannot wire up the UART (%d)", ret);
		return ret;
	}

#if defined(CONFIG_APP_SELFTEST)
	run_selftest(bridge.server());
	LOG_INF("Self-test done, now serving the UART forever");
#endif

	/* Everything from here on is driven by the UART interrupt. */
	return 0;
}
