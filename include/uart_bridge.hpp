/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Wire a UART straight into a RawHttpServer.
 *
 * Bytes received on the UART become HTTP requests, and the HTTP responses go
 * back out of the same UART. Nothing sits in between.
 *
 * RawHttpServer knows nothing about this class: it only ever sees buffers.
 */

#ifndef UART_BRIDGE_HPP_
#define UART_BRIDGE_HPP_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include "raw_http_server.hpp"

class UartBridge {
public:
	/**
	 * @param uart The UART carrying the HTTP byte stream.
	 * @param fs_root Already-mounted directory files are served from.
	 */
	UartBridge(const struct device *uart, const char *fs_root)
		: _uart(uart), _server(on_server_output, fs_root, this)
	{
	}

	UartBridge(const UartBridge &) = delete;
	UartBridge &operator=(const UartBridge &) = delete;

	/**
	 * @brief Attach the server to the UART and start serving.
	 *
	 * @return 0 on success, negative errno otherwise.
	 */
	int start();

	/** @brief The underlying server, for injecting requests directly. */
	RawHttpServer &server()
	{
		return _server;
	}

private:
	/**
	 * @brief Server -> UART trampoline, matching RawHttpServer::OutputCallback.
	 *
	 * Recovers the UartBridge from @p user_data and forwards to
	 * write_to_uart().
	 */
	static void on_server_output(const uint8_t *data, size_t len, void *user_data);

	/**
	 * @brief Transmit response bytes on the UART.
	 *
	 * Runs on whichever thread called RawHttpServer::input(), so blocking
	 * is allowed. uart_poll_out() busy-waits per byte, which is fine at
	 * sane baud rates and keeps this portable; switch to interrupt-driven
	 * TX with a second ring buffer to push large responses at high speed.
	 */
	void write_to_uart(const uint8_t *data, size_t len);

	/**
	 * @brief UART ISR trampoline, matching uart_irq_callback_user_data_t.
	 */
	static void isr_trampoline(const struct device *dev, void *user_data);

	/**
	 * @brief UART ISR: move received bytes into the RX ring.
	 *
	 * Only moves bytes and gives the semaphore - the server may block on
	 * file I/O, which must never happen in an ISR. A full ring drops
	 * bytes (loudly): dropping beats blocking here.
	 */
	void isr(const struct device *dev);

	/**
	 * @brief Feed thread trampoline, matching k_thread_entry_t.
	 */
	static void feed_trampoline(void *p1, void *p2, void *p3);

	/**
	 * @brief Drain the RX ring into the server, forever.
	 *
	 * Sleeps on the semaphore until the ISR signals new bytes, then hands
	 * them to RawHttpServer::input() - which does the parsing, the file
	 * I/O and the response transmission before it returns.
	 */
	void feed_loop();

	const struct device *_uart;
	RawHttpServer _server;
	struct ring_buf _rx_ring {};
	/** Absorbs bytes arriving while input() is busy with file I/O. */
	uint8_t _rx_ring_buf[CONFIG_RAW_HTTP_UART_RING_SIZE]{};
	struct k_sem _rx_sem {};
	struct k_thread _feed_thread {};
};

#endif /* UART_BRIDGE_HPP_ */
