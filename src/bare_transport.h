/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bare (network-less) transport for the Zephyr HTTP server.
 *
 * The HTTP server always talks to its peers through a socket, but it never
 * cares what is behind that socket. This module supplies:
 *
 *  - a minimal listening socket, handed to the server through
 *    @ref http_service_config::socket_create, whose accept() returns one end
 *    of a socketpair(2) instead of a TCP connection;
 *  - a byte-oriented API for the application, so that it can push raw request
 *    bytes in and receive raw response bytes out without ever touching a
 *    network stack.
 *
 * No L2 driver, no IP, no TCP is involved at any point.
 */

#ifndef BARE_TRANSPORT_H_
#define BARE_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/net/http/service.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to a bare connection. */
struct bare_conn;

/**
 * @brief Callback delivering raw bytes produced by the HTTP server.
 *
 * This is the "data out of the server" placeholder. Replace the body of the
 * callback you register with whatever moves bytes on your real link (UART,
 * USB, shared memory, a test harness, ...).
 *
 * @note Invoked from the connection RX thread, never from the HTTP server
 *       thread. The data is a byte stream, so a single HTTP response may be
 *       reported through several calls, and one call may carry the tail of one
 *       response and the head of the next.
 *
 * @param data Bytes emitted by the server, or NULL when the connection ended.
 * @param len Number of valid bytes in @p data, 0 when the connection ended.
 * @param user_data Opaque value passed to @ref bare_http_conn_open.
 */
typedef void (*bare_http_out_cb_t)(const uint8_t *data, size_t len, void *user_data);

/**
 * @brief Open a bare connection to the HTTP server.
 *
 * Creates a socketpair, queues one end on the bare listening socket so that
 * the HTTP server accepts it as a new client, and spawns a thread that pumps
 * the other end into @p out_cb.
 *
 * @param out_cb Callback receiving the raw response bytes. Must not be NULL.
 * @param user_data Opaque value forwarded to @p out_cb.
 *
 * @return Connection handle, or NULL on error.
 */
struct bare_conn *bare_http_conn_open(bare_http_out_cb_t out_cb, void *user_data);

/**
 * @brief Push raw request bytes into the HTTP server.
 *
 * This is the "data into the server" placeholder. Feed it whatever your real
 * link received; it does not have to be a whole request, and it may span
 * several requests.
 *
 * Blocks until every byte has been handed over, which can happen if the server
 * is slower than the caller. Never call it from @ref bare_http_out_cb_t.
 *
 * @param conn Connection handle from @ref bare_http_conn_open.
 * @param data Raw request bytes.
 * @param len Number of bytes in @p data.
 *
 * @return 0 on success, negative errno otherwise.
 */
int bare_http_input(struct bare_conn *conn, const void *data, size_t len);

/**
 * @brief Close a bare connection.
 *
 * @param conn Connection handle from @ref bare_http_conn_open.
 */
void bare_http_conn_close(struct bare_conn *conn);

/**
 * @brief Listening socket factory for the HTTP server.
 *
 * Install it in a @ref http_service_config passed to HTTP_SERVICE_DEFINE().
 * The returned descriptor accepts bind()/listen()/poll()/accept() but is not
 * bound to any address; the @p af and @p proto arguments are ignored.
 */
int bare_http_listener_create(const struct http_service_desc *svc, int af, int proto);

#ifdef __cplusplus
}
#endif

#endif /* BARE_TRANSPORT_H_ */
