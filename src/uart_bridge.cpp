/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "uart_bridge.hpp"

LOG_MODULE_REGISTER(uart_bridge, LOG_LEVEL_INF);

#define UART_FEED_CHUNK    512
#define UART_FEED_STACK    2048
#define UART_FEED_PRIORITY K_PRIO_PREEMPT(9)

K_THREAD_STACK_DEFINE(uart_feed_stack, UART_FEED_STACK);

void UartBridge::onServerOutput(const uint8_t *data, size_t len, void *user_data)
{
	static_cast<UartBridge *>(user_data)->writeToUart(data, len);
}

void UartBridge::writeToUart(const uint8_t *data, size_t len)
{
	if (data == nullptr) {
		/* The server hung up. input() relinks on the next byte that
		 * arrives, so there is nothing to do here.
		 */
		LOG_DBG("Server closed the connection, will relink on next input");
		return;
	}

	/* Runs on the server's RX thread, so blocking is allowed.
	 * uart_poll_out() busy-waits per byte, which is fine at sane baud rates
	 * and keeps this portable; switch to interrupt-driven TX with a second
	 * ring buffer if you push large responses at high speed.
	 */
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_, data[i]);
	}
}

void UartBridge::isrTrampoline(const struct device *dev, void *user_data)
{
	static_cast<UartBridge *>(user_data)->isr(dev);
}

void UartBridge::isr(const struct device *dev)
{
	uint8_t buf[32];

	if (!uart_irq_update(dev)) {
		return;
	}

	/* The ISR only moves bytes into the ring: RawHttpServer::input() may
	 * block when the server is slow, which must never happen here.
	 */
	while (uart_irq_rx_ready(dev)) {
		int read = uart_fifo_read(dev, buf, sizeof(buf));

		if (read <= 0) {
			break;
		}

		if (ring_buf_put(&rxRing_, buf, read) < static_cast<uint32_t>(read)) {
			/* Dropping beats blocking here, but it desynchronises
			 * the request stream, so make it loud.
			 */
			LOG_ERR("UART RX ring full, %d bytes dropped", read);
		}
	}

	k_sem_give(&rxSem_);
}

void UartBridge::feedTrampoline(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static_cast<UartBridge *>(p1)->feedLoop();
}

void UartBridge::feedLoop()
{
	uint8_t buf[UART_FEED_CHUNK];

	while (true) {
		uint32_t len;

		k_sem_take(&rxSem_, K_FOREVER);

		while ((len = ring_buf_get(&rxRing_, buf, sizeof(buf))) > 0) {
			int ret = server_.input(buf, len);

			if (ret < 0) {
				LOG_ERR("Feeding the HTTP server failed (%d)", ret);
			}
		}
	}
}

int UartBridge::start()
{
	int ret;

	if (!device_is_ready(uart_)) {
		LOG_ERR("UART %s is not ready", uart_->name);
		return -ENODEV;
	}

	ring_buf_init(&rxRing_, sizeof(rxRingBuf_), rxRingBuf_);
	k_sem_init(&rxSem_, 0, 1);

	ret = server_.start();
	if (ret < 0) {
		return ret;
	}

	k_thread_create(&feedThread_, uart_feed_stack, K_THREAD_STACK_SIZEOF(uart_feed_stack),
			feedTrampoline, this, nullptr, nullptr, UART_FEED_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&feedThread_, "uart_feed");

	uart_irq_callback_user_data_set(uart_, isrTrampoline, this);
	uart_irq_rx_enable(uart_);

	LOG_INF("UART %s wired to the HTTP server", uart_->name);

	return 0;
}
