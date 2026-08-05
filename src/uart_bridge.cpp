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
/* input() runs the parser, the filesystem and the response TX on this stack. */
#define UART_FEED_STACK    4096
#define UART_FEED_PRIORITY K_PRIO_PREEMPT(9)

K_THREAD_STACK_DEFINE(uart_feed_stack, UART_FEED_STACK);

void UartBridge::on_server_output(const uint8_t *data, size_t len, void *user_data)
{
	static_cast<UartBridge *>(user_data)->write_to_uart(data, len);
}

void UartBridge::write_to_uart(const uint8_t *data, size_t len)
{
	/* Runs inside RawHttpServer::input() on the feed thread, so blocking
	 * per byte is acceptable - and while a response drains here, arriving
	 * request bytes pile up safely in the RX ring.
	 */
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(_uart, data[i]);
	}
}

void UartBridge::isr_trampoline(const struct device *dev, void *user_data)
{
	static_cast<UartBridge *>(user_data)->isr(dev);
}

void UartBridge::isr(const struct device *dev)
{
	uint8_t buf[32];

	if (!uart_irq_update(dev)) {
		return;
	}

	/* Only move bytes and signal: the server blocks on file I/O, which
	 * must never happen in an ISR, so all real work is deferred to the
	 * feed thread.
	 */
	while (uart_irq_rx_ready(dev)) {
		int read = uart_fifo_read(dev, buf, sizeof(buf));

		if (read <= 0) {
			break;
		}

		if (ring_buf_put(&_rx_ring, buf, read) < static_cast<uint32_t>(read)) {
			/* Dropping beats blocking here, but it desynchronises
			 * the request stream, so make it loud.
			 */
			LOG_ERR("UART RX ring full, %d bytes dropped", read);
		}
	}

	k_sem_give(&_rx_sem);
}

void UartBridge::feed_trampoline(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static_cast<UartBridge *>(p1)->feed_loop();
}

void UartBridge::feed_loop()
{
	uint8_t buf[UART_FEED_CHUNK];

	while (true) {
		uint32_t len;

		/* Park until the ISR signals new bytes. The semaphore is a
		 * plain binary flag: one give may cover many ISR bursts, which
		 * is fine because the inner loop drains the ring completely.
		 */
		k_sem_take(&_rx_sem, K_FOREVER);

		while ((len = ring_buf_get(&_rx_ring, buf, sizeof(buf))) > 0) {
			/* input() parses, does the file I/O and transmits the
			 * response before returning - request boundaries do
			 * not matter, whatever fits in buf is handed over.
			 */
			int ret = _server.input(buf, len);

			if (ret < 0) {
				LOG_ERR("Feeding the HTTP server failed (%d)", ret);
			}
		}
	}
}

int UartBridge::start()
{
	if (!device_is_ready(_uart)) {
		LOG_ERR("UART %s is not ready", _uart->name);
		return -ENODEV;
	}

	ring_buf_init(&_rx_ring, sizeof(_rx_ring_buf), _rx_ring_buf);
	k_sem_init(&_rx_sem, 0, 1);

	/* The consumer must exist before the producer: start the feed thread
	 * first, then let the ISR begin filling the ring.
	 */
	k_thread_create(&_feed_thread, uart_feed_stack, K_THREAD_STACK_SIZEOF(uart_feed_stack),
			feed_trampoline, this, nullptr, nullptr, UART_FEED_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&_feed_thread, "uart_feed");

	uart_irq_callback_user_data_set(_uart, isr_trampoline, this);
	uart_irq_rx_enable(_uart);

	LOG_INF("UART %s wired to the HTTP server", _uart->name);

	return 0;
}
