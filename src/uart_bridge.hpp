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

#include "raw_http.hpp"

/** Bytes buffered between the UART ISR and the feed thread. */
#define UART_BRIDGE_RING_SIZE 1024

class UartBridge {
public:
	/**
	 * @param uart The UART carrying the HTTP byte stream.
	 * @param fs_root Already-mounted directory files are served from.
	 */
	UartBridge(const struct device *uart, const char *fs_root)
		: uart_(uart), server_(onServerOutput, fs_root, this)
	{
	}

	UartBridge(const UartBridge &) = delete;
	UartBridge &operator=(const UartBridge &) = delete;

	/**
	 * @brief Start the HTTP server and attach it to the UART.
	 *
	 * @return 0 on success, negative errno otherwise.
	 */
	int start();

	/** @brief The underlying server, for injecting requests directly. */
	RawHttpServer &server()
	{
		return server_;
	}

private:
	/* Server -> UART. Static, to match RawHttpServer::OutputCallback. */
	static void onServerOutput(const uint8_t *data, size_t len, void *user_data);
	void writeToUart(const uint8_t *data, size_t len);

	/* UART -> server. */
	static void isrTrampoline(const struct device *dev, void *user_data);
	void isr(const struct device *dev);
	static void feedTrampoline(void *p1, void *p2, void *p3);
	void feedLoop();

	const struct device *uart_;
	RawHttpServer server_;
	struct ring_buf rxRing_ {};
	uint8_t rxRingBuf_[UART_BRIDGE_RING_SIZE]{};
	struct k_sem rxSem_ {};
	struct k_thread feedThread_ {};
};

#endif /* UART_BRIDGE_HPP_ */
