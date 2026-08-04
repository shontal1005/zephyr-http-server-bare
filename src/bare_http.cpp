/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/fdtable.h>

#include "bare_http.hpp"

LOG_MODULE_REGISTER(bare_http, LOG_LEVEL_INF);

#define BARE_MAX_PENDING   2
#define BARE_RX_STACK_SIZE 1024
#define BARE_RX_PRIORITY   K_PRIO_PREEMPT(8)

/* How long poll() parks before rechecking running_. Bounds how long stop()
 * takes; an idle link costs one wakeup per interval and nothing else.
 */
#define BARE_POLL_TIMEOUT_MS 500

K_THREAD_STACK_DEFINE(bare_rx_stack, BARE_RX_STACK_SIZE);

namespace
{

/*
 * The listening socket.
 *
 * The HTTP server only ever does four things with it: setsockopt(), bind(),
 * listen(), then poll()+accept() in a loop. bind() and listen() are no-ops
 * here, poll() is backed by a k_poll_signal, and accept() pops a socketpair end
 * that BareHttpServer::link() queued.
 *
 * It is deliberately a singleton: HTTP_SERVICE_DEFINE() registers exactly one
 * service, and this sample serves exactly one client.
 */
struct Listener {
	struct k_poll_signal acceptSig;
	struct k_spinlock lock;
	int pending[BARE_MAX_PENDING];
	uint8_t pendingCount;
	bool inUse;
};

Listener listener;
struct socket_op_vtable listenerVtable;

ssize_t listenerRead(void *obj, void *buf, size_t sz)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(buf);
	ARG_UNUSED(sz);

	errno = EOPNOTSUPP;
	return -1;
}

ssize_t listenerWrite(void *obj, const void *buf, size_t sz)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(buf);
	ARG_UNUSED(sz);

	errno = EOPNOTSUPP;
	return -1;
}

int listenerClose(void *obj)
{
	int fds[BARE_MAX_PENDING];
	uint8_t count;

	ARG_UNUSED(obj);

	k_spinlock_key_t key = k_spin_lock(&listener.lock);

	count = listener.pendingCount;
	memcpy(fds, listener.pending, count * sizeof(fds[0]));
	listener.pendingCount = 0;
	listener.inUse = false;

	k_spin_unlock(&listener.lock, key);

	/* Drop connections that were queued but never accepted. */
	for (uint8_t i = 0; i < count; i++) {
		(void)zsock_close(fds[i]);
	}

	return 0;
}

int listenerPollPrepare(struct zsock_pollfd *pfd, struct k_poll_event **pev,
			struct k_poll_event *pev_end)
{
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
	k_spinlock_key_t key = k_spin_lock(&listener.lock);

	if (listener.pendingCount == 0) {
		k_poll_signal_reset(&listener.acceptSig);
	}

	k_spin_unlock(&listener.lock, key);

	(*pev)->obj = &listener.acceptSig;
	(*pev)->type = K_POLL_TYPE_SIGNAL;
	(*pev)->mode = K_POLL_MODE_NOTIFY_ONLY;
	(*pev)->state = K_POLL_STATE_NOT_READY;
	(*pev)++;

	return 0;
}

int listenerPollUpdate(struct zsock_pollfd *pfd, struct k_poll_event **pev)
{
	if ((pfd->events & ZSOCK_POLLIN) == 0) {
		return 0;
	}

	/* Readiness comes from the queue, not from the signal state. */
	k_spinlock_key_t key = k_spin_lock(&listener.lock);

	if (listener.pendingCount > 0) {
		pfd->revents |= ZSOCK_POLLIN;
	}

	k_spin_unlock(&listener.lock, key);

	(*pev)++;

	return 0;
}

int listenerIoctl(void *obj, unsigned int request, va_list args)
{
	ARG_UNUSED(obj);

	switch (request) {
	case ZFD_IOCTL_POLL_PREPARE: {
		struct zsock_pollfd *pfd = va_arg(args, struct zsock_pollfd *);
		struct k_poll_event **pev = va_arg(args, struct k_poll_event **);
		struct k_poll_event *pev_end = va_arg(args, struct k_poll_event *);

		return listenerPollPrepare(pfd, pev, pev_end);
	}

	case ZFD_IOCTL_POLL_UPDATE: {
		struct zsock_pollfd *pfd = va_arg(args, struct zsock_pollfd *);
		struct k_poll_event **pev = va_arg(args, struct k_poll_event **);

		return listenerPollUpdate(pfd, pev);
	}

	default:
		errno = EOPNOTSUPP;
		return -1;
	}
}

int listenerBind(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(addr);
	ARG_UNUSED(addrlen);

	/* There is no address space to bind to; accept the call so that
	 * http_server_init() keeps going.
	 */
	return 0;
}

int listenerListen(void *obj, int backlog)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(backlog);

	return 0;
}

int listenerAccept(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	int fd;

	ARG_UNUSED(obj);
	ARG_UNUSED(addr);

	k_spinlock_key_t key = k_spin_lock(&listener.lock);

	if (listener.pendingCount == 0) {
		k_spin_unlock(&listener.lock, key);
		errno = EAGAIN;
		return -1;
	}

	fd = listener.pending[0];
	listener.pendingCount--;
	memmove(&listener.pending[0], &listener.pending[1],
		listener.pendingCount * sizeof(listener.pending[0]));

	k_spin_unlock(&listener.lock, key);

	/* Report an empty peer address. The caller zeroes the storage first, so
	 * it stays NET_AF_UNSPEC.
	 */
	if (addrlen != nullptr) {
		*addrlen = 0;
	}

	return fd;
}

int listenerGetsockopt(void *obj, int level, int optname, void *optval, net_socklen_t *optlen)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(level);
	ARG_UNUSED(optname);

	/* Only ever reached through the server's SO_ERROR read on POLLERR,
	 * which this listener never reports.
	 */
	if (optval != nullptr && optlen != nullptr && *optlen >= sizeof(int)) {
		*static_cast<int *>(optval) = 0;
		*optlen = sizeof(int);
	}

	return 0;
}

int listenerSetsockopt(void *obj, int level, int optname, const void *optval, net_socklen_t optlen)
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

/* Filled in field by field rather than with a designated initialiser, because
 * fd_op_vtable puts read/write/close inside anonymous unions.
 */
void initListenerVtable()
{
	listenerVtable.fd_vtable.read = listenerRead;
	listenerVtable.fd_vtable.write = listenerWrite;
	listenerVtable.fd_vtable.close = listenerClose;
	listenerVtable.fd_vtable.ioctl = listenerIoctl;
	listenerVtable.bind = listenerBind;
	listenerVtable.listen = listenerListen;
	listenerVtable.accept = listenerAccept;
	listenerVtable.getsockopt = listenerGetsockopt;
	listenerVtable.setsockopt = listenerSetsockopt;
}

/* Queue an accepted descriptor and wake the server's poll() up. */
int listenerPush(int fd)
{
	k_spinlock_key_t key = k_spin_lock(&listener.lock);

	if (!listener.inUse) {
		k_spin_unlock(&listener.lock, key);
		return -ENOTCONN;
	}

	if (listener.pendingCount == ARRAY_SIZE(listener.pending)) {
		k_spin_unlock(&listener.lock, key);
		return -ENOSPC;
	}

	listener.pending[listener.pendingCount++] = fd;

	k_spin_unlock(&listener.lock, key);

	/* Must be raised outside the spinlock, it may reschedule. */
	k_poll_signal_raise(&listener.acceptSig, 0);

	return 0;
}

/* The running instance, so the RX thread trampoline can find it. */
BareHttpServer *instance;

} /* namespace */

int BareHttpServer::socketCreate(const struct http_service_desc *svc, int af, int proto)
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

	initListenerVtable();
	k_poll_signal_init(&listener.acceptSig);
	listener.pendingCount = 0;
	listener.inUse = true;

	zvfs_finalize_typed_fd(fd, &listener,
			       reinterpret_cast<const struct fd_op_vtable *>(&listenerVtable),
			       ZVFS_MODE_IFSOCK);

	LOG_DBG("Bare listening socket created (fd %d)", fd);

	return fd;
}

void BareHttpServer::rxTrampoline(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static_cast<BareHttpServer *>(p1)->rxLoop();
}

void BareHttpServer::rxLoop()
{
	bool eof = false;

	while (running_ && !eof) {
		struct zsock_pollfd pfd = {
			.fd = appFd_,
			.events = ZSOCK_POLLIN,
			.revents = 0,
		};

		/* The timeout is what lets stop() retire this thread; the fd
		 * must stay open while we are parked in poll().
		 */
		int ret = zsock_poll(&pfd, 1, BARE_POLL_TIMEOUT_MS);

		if (ret < 0) {
			LOG_DBG("poll failed (%d)", -errno);
			break;
		}

		if (ret == 0) {
			continue;
		}

		if ((pfd.revents & ZSOCK_POLLIN) == 0) {
			if (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP)) {
				break;
			}
			continue;
		}

		/* Short reads are normal: this delivers whatever is buffered,
		 * never a fixed-size frame.
		 */
		ssize_t got = zsock_recv(appFd_, rxBuf_, sizeof(rxBuf_), 0);

		if (got > 0) {
			cb_(rxBuf_, static_cast<size_t>(got), userData_);
			continue;
		}

		if (got == 0) {
			LOG_DBG("HTTP server closed the connection");
			eof = true;
		} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
			LOG_DBG("recv failed (%d)", -errno);
			eof = true;
		}
	}

	/* The next input() relinks transparently. */
	linked_ = false;

	/* End-of-stream marker for the application. */
	cb_(nullptr, 0, userData_);
}

/* Caller must hold lock_. */
int BareHttpServer::link()
{
	int sv[2];
	int ret;

	/* A previous RX thread may still be unwinding. It must be fully gone
	 * before its stack is handed to a new one.
	 */
	if (threadStarted_) {
		(void)k_thread_join(&thread_, K_FOREVER);
		threadStarted_ = false;
	}

	ret = zsock_socketpair(NET_AF_UNIX, NET_SOCK_STREAM, 0, sv);
	if (ret < 0) {
		return -errno;
	}

	/*
	 * Our end must be non-blocking. Every zsock_*() call holds a per-fd
	 * mutex for its whole duration (VTABLE_CALL in subsys/net/lib/sockets/
	 * sockets.c), so a blocking recv() parked on this descriptor would lock
	 * out send() on the same descriptor from any other thread - a hard
	 * deadlock as soon as one connection serves more than one request.
	 * With O_NONBLOCK nothing blocks inside the mutex, and poll() - which
	 * does not hold it while waiting - provides the blocking instead.
	 *
	 * The server's end stays blocking: that is what its own code expects.
	 */
	if (zsock_fcntl(sv[0], ZVFS_F_SETFL, ZVFS_O_NONBLOCK) < 0) {
		ret = -errno;
		(void)zsock_close(sv[0]);
		(void)zsock_close(sv[1]);
		return ret;
	}

	/* sv[1] becomes the HTTP server's client socket, sv[0] stays with us. */
	ret = listenerPush(sv[1]);
	if (ret < 0) {
		(void)zsock_close(sv[0]);
		(void)zsock_close(sv[1]);
		return ret;
	}

	appFd_ = sv[0];
	linked_ = true;
	running_ = true;

	k_thread_create(&thread_, bare_rx_stack, K_THREAD_STACK_SIZEOF(bare_rx_stack),
			rxTrampoline, this, nullptr, nullptr, BARE_RX_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&thread_, "bare_rx");
	threadStarted_ = true;

	return 0;
}

int BareHttpServer::start()
{
	int ret;

	if (cb_ == nullptr) {
		return -EINVAL;
	}

	if (started_) {
		return 0;
	}

	if (instance != nullptr) {
		LOG_ERR("Another BareHttpServer is already running");
		return -EEXIST;
	}

	ret = http_server_start();
	if (ret < 0) {
		LOG_ERR("Cannot start the HTTP server (%d)", ret);
		return ret;
	}

	k_mutex_init(&lock_);
	instance = this;
	started_ = true;

	/* Let the server thread reach its poll() and create the listening
	 * socket before queueing a connection onto it.
	 */
	k_sleep(K_MSEC(100));

	k_mutex_lock(&lock_, K_FOREVER);
	ret = link();
	k_mutex_unlock(&lock_);

	if (ret < 0) {
		LOG_ERR("Cannot establish the connection (%d)", ret);
		instance = nullptr;
		started_ = false;
		return ret;
	}

	return 0;
}

int BareHttpServer::input(const void *buffer, size_t size)
{
	const uint8_t *ptr = static_cast<const uint8_t *>(buffer);
	int ret = 0;

	if (!started_) {
		return -ENOTCONN;
	}

	k_mutex_lock(&lock_, K_FOREVER);

	/* The server hangs up on a malformed request, and on its inactivity
	 * timeout where the transport supports shutdown(). Relink so the link
	 * keeps serving instead of going dead for good.
	 */
	if (!linked_) {
		LOG_INF("Reconnecting to the HTTP server");
		ret = link();
		if (ret < 0) {
			LOG_ERR("Relink failed (%d)", ret);
			goto out;
		}
	}

	while (size > 0) {
		ssize_t sent = zsock_send(appFd_, ptr, size, 0);

		if (sent > 0) {
			ptr += sent;
			size -= sent;
			continue;
		}

		if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			ret = -errno;
			goto out;
		}

		/* The server has not drained its side yet. Wait for room here
		 * rather than inside the socket call, so the RX thread can keep
		 * draining responses meanwhile.
		 */
		struct zsock_pollfd pfd = {
			.fd = appFd_,
			.events = ZSOCK_POLLOUT,
			.revents = 0,
		};

		if (zsock_poll(&pfd, 1, BARE_POLL_TIMEOUT_MS) < 0) {
			ret = -errno;
			goto out;
		}

		if (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP)) {
			ret = -ECONNRESET;
			goto out;
		}
	}

out:
	k_mutex_unlock(&lock_);

	return ret;
}

void BareHttpServer::stop()
{
	if (!started_) {
		return;
	}

	k_mutex_lock(&lock_, K_FOREVER);

	started_ = false;
	linked_ = false;

	/* Retire the RX thread first: it is parked in poll() on appFd_, and the
	 * descriptor must stay valid until it is gone. It notices within one
	 * poll timeout.
	 */
	running_ = false;

	if (threadStarted_) {
		(void)k_thread_join(&thread_, K_FOREVER);
		threadStarted_ = false;
	}

	(void)zsock_close(appFd_);
	appFd_ = -1;

	instance = nullptr;

	k_mutex_unlock(&lock_);
}

BareHttpServer::~BareHttpServer()
{
	stop();
}
