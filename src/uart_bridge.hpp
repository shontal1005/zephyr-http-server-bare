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
 * RawHttpServer knows nothing about this class: it only ever sees packets.
 */

#ifndef UART_BRIDGE_HPP_
#define UART_BRIDGE_HPP_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include <raw_http_server.hpp>

/** Bytes buffered between the UART ISR and the feed thread. */
#define UART_BRIDGE_RING_SIZE 4096

class UartBridge {
 public:
  /**
   * @param uart The UART carrying the HTTP byte stream.
   * @param pool Pool the request packets are allocated from; the
   *             buffer size only sets how RX bytes are chunked. The
   *             server frees each packet when done with it.
   */
  UartBridge(const struct device* uart, struct net_buf_pool* pool)
      : _uart(uart), _pool(pool), _server(on_server_output, this) {}

  UartBridge(const UartBridge&) = delete;
  UartBridge& operator=(const UartBridge&) = delete;

  /**
   * @brief Attach the server to the UART and start serving.
   *
   * @return 0 on success, negative errno otherwise.
   */
  int start();

  /** @brief The underlying server, for injecting requests directly. */
  RawHttpServer& server() { return _server; }

 private:
  /**
   * @brief Server -> UART trampoline, matching RawHttpServer::OutputCallback.
   *
   * Recovers the UartBridge from @p user_data and forwards to
   * write_to_uart().
   */
  static void on_server_output(const uint8_t* data,
                               size_t len,
                               void* user_data);

  /**
   * @brief Transmit response bytes on the UART.
   *
   * Runs on the server's thread, so blocking is allowed - and blocking
   * is the flow control: the server cannot produce response bytes any
   * faster than the wire drains them. uart_poll_out() busy-waits per
   * byte, which is fine at sane baud rates and keeps this portable;
   * switch to interrupt-driven TX with a ring and a semaphore to give
   * the CPU back while a large response drains.
   */
  void write_to_uart(const uint8_t* data, size_t len);

  /**
   * @brief UART ISR trampoline, matching uart_irq_callback_user_data_t.
   */
  static void isr_trampoline(const struct device* dev, void* user_data);

  /**
   * @brief UART ISR: move received bytes into the RX ring.
   *
   * Only moves bytes and gives the semaphore - allocating a packet may
   * block on an exhausted pool, which must never happen in an ISR. A
   * full ring drops bytes (loudly): dropping beats blocking here.
   */
  void isr(const struct device* dev);

  /**
   * @brief Feed thread trampoline, matching k_thread_entry_t.
   */
  static void feed_trampoline(void* p1, void* p2, void* p3);

  /**
   * @brief Package the RX ring into packets for the server, forever.
   *
   * Sleeps on the semaphore until the ISR signals new bytes, then wraps
   * them in net_bufs and hands them to enqueue_packet(). The server's
   * own thread does the parsing, the file I/O and the response
   * transmission - packets queue up while it is busy.
   */
  void feed_loop();

  const struct device* _uart;
  struct net_buf_pool* _pool;
  RawHttpServer _server;
  struct ring_buf _rx_ring{};
  /** Absorbs bytes arriving while the packet pool is exhausted. */
  uint8_t _rx_ring_buf[UART_BRIDGE_RING_SIZE]{};
  /** ISR sets this on ring overflow; the feed thread sends the marker. */
  atomic_t _rx_overflowed{0};
  struct k_sem _rx_sem{};
  struct k_thread _feed_thread{};
};

#endif /* UART_BRIDGE_HPP_ */
