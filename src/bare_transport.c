/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/fdtable.h>

#include "bare_transport.h"

LOG_MODULE_REGISTER(bare_transport, LOG_LEVEL_INF);

/* Only one bare connection at a time keeps the sample readable. Raise both
 * values (and CONFIG_HTTP_SERVER_MAX_CLIENTS) if you want more.
 */
#define BARE_MAX_CONN    1
#define BARE_MAX_PENDING 2

#define BARE_RX_BUF_SIZE   1600
#define BARE_RX_STACK_SIZE 1024
#define BARE_RX_PRIORITY   K_PRIO_PREEMPT(8)

/*
 * The listening socket.
 *
 * The HTTP server only ever does four things with it: setsockopt(), bind(),
 * listen() and then poll()+accept() in a loop. bind() and listen() are
 * no-ops here, poll() is backed by a k_poll_signal, and accept() pops a
 * socketpair end that the application queued from bare_http_conn_open().
 */
struct bare_listener {
	struct k_poll_signal accept_sig;
	struct k_spinlock lock;
	int pending[BARE_MAX_PENDING];
	uint8_t pending_count;
	bool in_use;
};

static struct bare_listener listener;

struct bare_conn {
	int app_fd;
	bare_http_out_cb_t out_cb;
	void *user_data;
	struct k_thread thread;
	/* Kept out of the RX thread stack, which would not survive it. */
	uint8_t rx_buf[BARE_RX_BUF_SIZE];
	bool in_use;
};

static struct bare_conn conns[BARE_MAX_CONN];
static K_THREAD_STACK_ARRAY_DEFINE(rx_stacks, BARE_MAX_CONN, BARE_RX_STACK_SIZE);

/* Queue an accepted descriptor and wake up the server's poll(). */
static int listener_push(int fd)
{
	k_spinlock_key_t key;

	key = k_spin_lock(&listener.lock);

	if (!listener.in_use) {
		k_spin_unlock(&listener.lock, key);
		return -ENOTCONN;
	}

	if (listener.pending_count == ARRAY_SIZE(listener.pending)) {
		k_spin_unlock(&listener.lock, key);
		return -ENOSPC;
	}

	listener.pending[listener.pending_count++] = fd;

	k_spin_unlock(&listener.lock, key);

	/* Must be raised outside of the spinlock, it may reschedule. */
	k_poll_signal_raise(&listener.accept_sig, 0);

	return 0;
}

static int listener_poll_prepare(struct zsock_pollfd *pfd, struct k_poll_event **pev,
				 struct k_poll_event *pev_end)
{
	k_spinlock_key_t key;

	if ((pfd->events & ZSOCK_POLLIN) == 0) {
		return 0;
	}

	if (*pev == pev_end) {
		errno = ENOMEM;
		return -1;
	}

	/* Clear a stale signal before blocking. Anything queued after this
	 * point raises the signal again and wakes the poll up.
	 */
	key = k_spin_lock(&listener.lock);
	if (listener.pending_count == 0) {
		k_poll_signal_reset(&listener.accept_sig);
	}
	k_spin_unlock(&listener.lock, key);

	(*pev)->obj = &listener.accept_sig;
	(*pev)->type = K_POLL_TYPE_SIGNAL;
	(*pev)->mode = K_POLL_MODE_NOTIFY_ONLY;
	(*pev)->state = K_POLL_STATE_NOT_READY;
	(*pev)++;

	return 0;
}

static int listener_poll_update(struct zsock_pollfd *pfd, struct k_poll_event **pev)
{
	k_spinlock_key_t key;

	if ((pfd->events & ZSOCK_POLLIN) == 0) {
		return 0;
	}

	/* Readiness comes from the queue, not from the signal state. */
	key = k_spin_lock(&listener.lock);
	if (listener.pending_count > 0) {
		pfd->revents |= ZSOCK_POLLIN;
	}
	k_spin_unlock(&listener.lock, key);

	(*pev)++;

	return 0;
}

static ssize_t listener_read(void *obj, void *buf, size_t sz)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(buf);
	ARG_UNUSED(sz);

	errno = EOPNOTSUPP;
	return -1;
}

static ssize_t listener_write(void *obj, const void *buf, size_t sz)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(buf);
	ARG_UNUSED(sz);

	errno = EOPNOTSUPP;
	return -1;
}

static int listener_close(void *obj)
{
	int fds[BARE_MAX_PENDING];
	k_spinlock_key_t key;
	uint8_t count;

	ARG_UNUSED(obj);

	key = k_spin_lock(&listener.lock);
	count = listener.pending_count;
	memcpy(fds, listener.pending, count * sizeof(fds[0]));
	listener.pending_count = 0;
	listener.in_use = false;
	k_spin_unlock(&listener.lock, key);

	/* Drop connections that were queued but never accepted. */
	for (uint8_t i = 0; i < count; i++) {
		(void)zsock_close(fds[i]);
	}

	return 0;
}

static int listener_ioctl(void *obj, unsigned int request, va_list args)
{
	ARG_UNUSED(obj);

	switch (request) {
	case ZFD_IOCTL_POLL_PREPARE: {
		struct zsock_pollfd *pfd;
		struct k_poll_event **pev;
		struct k_poll_event *pev_end;

		pfd = va_arg(args, struct zsock_pollfd *);
		pev = va_arg(args, struct k_poll_event **);
		pev_end = va_arg(args, struct k_poll_event *);

		return listener_poll_prepare(pfd, pev, pev_end);
	}

	case ZFD_IOCTL_POLL_UPDATE: {
		struct zsock_pollfd *pfd;
		struct k_poll_event **pev;

		pfd = va_arg(args, struct zsock_pollfd *);
		pev = va_arg(args, struct k_poll_event **);

		return listener_poll_update(pfd, pev);
	}

	default:
		errno = EOPNOTSUPP;
		return -1;
	}
}

static int listener_bind(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(addr);
	ARG_UNUSED(addrlen);

	/* There is no address space to bind to; accept the call so that
	 * http_server_init() keeps going.
	 */
	return 0;
}

static int listener_listen(void *obj, int backlog)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(backlog);

	return 0;
}

static int listener_accept(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	k_spinlock_key_t key;
	int fd;

	ARG_UNUSED(obj);

	key = k_spin_lock(&listener.lock);

	if (listener.pending_count == 0) {
		k_spin_unlock(&listener.lock, key);
		errno = EAGAIN;
		return -1;
	}

	fd = listener.pending[0];
	listener.pending_count--;
	memmove(&listener.pending[0], &listener.pending[1],
		listener.pending_count * sizeof(listener.pending[0]));

	k_spin_unlock(&listener.lock, key);

	/* Report an empty peer address. The caller zeroes the storage first,
	 * so it stays NET_AF_UNSPEC.
	 */
	ARG_UNUSED(addr);
	if (addrlen != NULL) {
		*addrlen = 0;
	}

	return fd;
}

static int listener_getsockopt(void *obj, int level, int optname, void *optval,
			       net_socklen_t *optlen)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(level);
	ARG_UNUSED(optname);

	/* Only ever reached through the server's SO_ERROR read on POLLERR,
	 * which this listener never reports.
	 */
	if (optval != NULL && optlen != NULL && *optlen >= sizeof(int)) {
		*(int *)optval = 0;
		*optlen = sizeof(int);
	}

	return 0;
}

static int listener_setsockopt(void *obj, int level, int optname, const void *optval,
			       net_socklen_t optlen)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(level);
	ARG_UNUSED(optname);
	ARG_UNUSED(optval);
	ARG_UNUSED(optlen);

	/* SO_REUSEADDR and friends are meaningless here, but the server
	 * abandons the service if setsockopt() fails.
	 */
	return 0;
}

static const struct socket_op_vtable listener_vtable = {
	.fd_vtable = {
		.read = listener_read,
		.write = listener_write,
		.close = listener_close,
		.ioctl = listener_ioctl,
	},
	.bind = listener_bind,
	.listen = listener_listen,
	.accept = listener_accept,
	.getsockopt = listener_getsockopt,
	.setsockopt = listener_setsockopt,
};

int bare_http_listener_create(const struct http_service_desc *svc, int af, int proto)
{
	int fd;

	ARG_UNUSED(svc);
	ARG_UNUSED(af);
	ARG_UNUSED(proto);

	fd = zvfs_reserve_fd();
	if (fd < 0) {
		errno = EMFILE;
		return -1;
	}

	k_poll_signal_init(&listener.accept_sig);
	listener.pending_count = 0;
	listener.in_use = true;

	zvfs_finalize_typed_fd(fd, &listener, (const struct fd_op_vtable *)&listener_vtable,
			       ZVFS_MODE_IFSOCK);

	LOG_DBG("Bare listening socket created (fd %d)", fd);

	return fd;
}

static void bare_rx_thread(void *p1, void *p2, void *p3)
{
	struct bare_conn *conn = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		/* Short reads are normal: this delivers whatever is buffered,
		 * never a fixed-size frame.
		 */
		ssize_t ret = zsock_recv(conn->app_fd, conn->rx_buf, sizeof(conn->rx_buf), 0);

		if (ret > 0) {
			conn->out_cb(conn->rx_buf, (size_t)ret, conn->user_data);
			continue;
		}

		if (ret == 0) {
			LOG_DBG("HTTP server closed the bare connection");
		} else {
			LOG_DBG("recv failed (%d)", -errno);
		}

		break;
	}

	/* End-of-stream marker for the application. */
	conn->out_cb(NULL, 0, conn->user_data);
}

struct bare_conn *bare_http_conn_open(bare_http_out_cb_t out_cb, void *user_data)
{
	struct bare_conn *conn = NULL;
	int sv[2];
	int ret;
	size_t i;

	if (out_cb == NULL) {
		return NULL;
	}

	for (i = 0; i < ARRAY_SIZE(conns); i++) {
		if (!conns[i].in_use) {
			conn = &conns[i];
			break;
		}
	}

	if (conn == NULL) {
		LOG_ERR("No free bare connection slot");
		return NULL;
	}

	ret = zsock_socketpair(NET_AF_UNIX, NET_SOCK_STREAM, 0, sv);
	if (ret < 0) {
		LOG_ERR("socketpair failed (%d)", -errno);
		return NULL;
	}

	/* sv[1] becomes the HTTP server's client socket, sv[0] stays with us. */
	ret = listener_push(sv[1]);
	if (ret < 0) {
		LOG_ERR("Cannot queue connection (%d)", ret);
		(void)zsock_close(sv[0]);
		(void)zsock_close(sv[1]);
		return NULL;
	}

	conn->app_fd = sv[0];
	conn->out_cb = out_cb;
	conn->user_data = user_data;
	conn->in_use = true;

	k_thread_create(&conn->thread, rx_stacks[i], K_THREAD_STACK_SIZEOF(rx_stacks[i]),
			bare_rx_thread, conn, NULL, NULL, BARE_RX_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&conn->thread, "bare_rx");

	return conn;
}

int bare_http_input(struct bare_conn *conn, const void *data, size_t len)
{
	const uint8_t *ptr = data;

	if (conn == NULL || !conn->in_use) {
		return -ENOTCONN;
	}

	while (len > 0) {
		ssize_t ret = zsock_send(conn->app_fd, ptr, len, 0);

		if (ret < 0) {
			return -errno;
		}

		ptr += ret;
		len -= ret;
	}

	return 0;
}

void bare_http_conn_close(struct bare_conn *conn)
{
	if (conn == NULL || !conn->in_use) {
		return;
	}

	conn->in_use = false;

	/* Unblocks the RX thread, which then exits on its own. */
	(void)zsock_close(conn->app_fd);
	conn->app_fd = -1;

	/* Wait for it before the slot (and its stack) can be reused. */
	(void)k_thread_join(&conn->thread, K_FOREVER);
}
