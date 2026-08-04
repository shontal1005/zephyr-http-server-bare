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
 * underneath it: no L2 driver, no IP, no TCP. Requests are handed over as raw
 * byte buffers through bare_http_input(), and responses come back as raw byte
 * buffers through the registered bare_http_out_cb_t.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>

#include "bare_transport.h"

LOG_MODULE_REGISTER(net_http_server_bare, LOG_LEVEL_INF);

/*
 * The HTTP server builds a sockaddr before it calls the socket_create hook,
 * so a service still needs a host and a port even though neither is used.
 * Keep the port non-zero: a zero port means "ephemeral", which would make the
 * server call getsockname() on the bare listening socket.
 */
static uint16_t bare_port = 80;

static const struct http_service_config bare_config = {
	.socket_create = bare_http_listener_create,
};

HTTP_SERVICE_DEFINE(bare_service, NULL, &bare_port, 1, 1, NULL, NULL, &bare_config);

/* A static resource, served straight out of flash. */
static const char index_html[] =
	"<html><body><h1>Zephyr HTTP server, no network attached</h1></body></html>";

static struct http_resource_detail_static index_html_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
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

	response_ctx->body_len = snprintf((char *)body, sizeof(body), "{\"uptime_ms\":%lld}\n",
					  k_uptime_get());
	response_ctx->body = body;
	response_ctx->final_chunk = true;

	return 0;
}

static struct http_resource_detail_dynamic uptime_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "application/json",
	},
	.cb = uptime_handler,
	.user_data = NULL,
};

HTTP_RESOURCE_DEFINE(uptime_resource, bare_service, "/uptime", &uptime_resource_detail);

/*
 * ---------------------------------------------------------------------------
 * Application side: raw bytes in, raw bytes out.
 * ---------------------------------------------------------------------------
 */

static K_SEM_DEFINE(response_done, 0, 1);

/**
 * @brief Placeholder for "the HTTP server produced these bytes".
 *
 * Replace the body with a write to your real link. Remember that this is a
 * byte stream: @p len is whatever happened to be available, not a message.
 */
static void on_server_data(const uint8_t *data, size_t len, void *user_data)
{
	ARG_UNUSED(user_data);

	if (data == NULL) {
		LOG_INF("<-- connection closed by the server");
		k_sem_give(&response_done);
		return;
	}

	LOG_INF("<-- %zu bytes from the HTTP server:", len);
	printk("%.*s", (int)len, data);
}

/**
 * @brief Placeholder for "my link received these bytes, hand them to the server".
 *
 * The request is split into two writes on purpose: the server does not care
 * about buffer boundaries, so it may arrive in as many pieces as you like.
 */
static void do_request(const char *request)
{
	size_t len = strlen(request);
	size_t first = len / 2;
	struct bare_conn *conn;
	int ret;

	conn = bare_http_conn_open(on_server_data, NULL);
	if (conn == NULL) {
		LOG_ERR("Cannot open a bare connection");
		return;
	}

	LOG_INF("--> feeding %zu bytes in two chunks", len);

	ret = bare_http_input(conn, request, first);
	if (ret == 0) {
		ret = bare_http_input(conn, request + first, len - first);
	}

	if (ret < 0) {
		LOG_ERR("bare_http_input failed (%d)", ret);
	} else if (k_sem_take(&response_done, K_SECONDS(5)) != 0) {
		/* The request carries "Connection: close", so the server hangs
		 * up once the response is out.
		 */
		LOG_WRN("Timed out waiting for the response");
	}

	bare_http_conn_close(conn);
}

int main(void)
{
	int ret;

	ret = http_server_start();
	if (ret < 0) {
		LOG_ERR("Cannot start the HTTP server (%d)", ret);
		return ret;
	}

	/* Let the server thread reach its poll() before queueing a connection.
	 * Queueing early is harmless, this only keeps the log output readable.
	 */
	k_sleep(K_MSEC(100));

	/* Static resource. */
	do_request("GET / HTTP/1.1\r\n"
		   "Host: bare\r\n"
		   "Connection: close\r\n"
		   "\r\n");

	/* Dynamic resource, handled by uptime_handler() above. */
	do_request("GET /uptime HTTP/1.1\r\n"
		   "Host: bare\r\n"
		   "Connection: close\r\n"
		   "\r\n");

	LOG_INF("Done");

	return 0;
}
