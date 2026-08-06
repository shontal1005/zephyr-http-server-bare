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

/* The feed thread only allocates packets and copies bytes; the parser, the
 * filesystem and the response TX all run on the server's own thread.
 */
#define UART_FEED_STACK 2048
#define UART_FEED_PRIORITY K_PRIO_PREEMPT(9)

K_THREAD_STACK_DEFINE(uart_feed_stack, UART_FEED_STACK);

void UartBridge::on_server_output(const uint8_t* data,
                                  size_t len,
                                  void* user_data) {
  static_cast<UartBridge*>(user_data)->write_to_uart(data, len);
}

void UartBridge::write_to_uart(const uint8_t* data, size_t len) {
  /* Blocking per byte IS the flow control: the server never outruns the
   * wire, and arriving bytes pile up safely in the ring and the queue.
   */
  for (size_t i = 0; i < len; i++) {
    uart_poll_out(_uart, data[i]);
  }
}

void UartBridge::isr_trampoline(const struct device* dev, void* user_data) {
  static_cast<UartBridge*>(user_data)->isr(dev);
}

void UartBridge::isr(const struct device* dev) {
  uint8_t buf[32];

  if (uart_irq_update(dev) <= 0) {
    return;
  }

  /* Only move bytes and signal: a packet alloc may block, never OK here. */
  while (uart_irq_rx_ready(dev) > 0) {
    int read = uart_fifo_read(dev, buf, sizeof(buf));

    if (read <= 0) {
      break;
    }

    uint32_t put = ring_buf_put(&_rx_ring, buf, read);

    if (put < static_cast<uint32_t>(read)) {
      /* Dropping beats blocking. The mangled request answers for
       * itself: it cannot parse, so the server 400s its packet.
       */
      LOG_ERR("UART RX ring full, %u bytes dropped",
              static_cast<uint32_t>(read) - put);
    }
  }

  k_sem_give(&_rx_sem);
}

void UartBridge::feed_trampoline(void* p1, void* p2, void* p3) {
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  static_cast<UartBridge*>(p1)->feed_loop();
}

void UartBridge::feed_loop() {
  while (true) {
    /* One give may cover many ISR bursts; the loop drains completely. */
    k_sem_take(&_rx_sem, K_FOREVER);

    while (!ring_buf_is_empty(&_rx_ring)) {
      /* Blocking on an exhausted pool is safe: the ring absorbs. */
      struct net_buf* packet = net_buf_alloc(_pool, K_FOREVER);
      uint32_t len = ring_buf_get(&_rx_ring, packet->data, packet->size);

      if (len == 0) {
        net_buf_unref(packet);
        break;
      }

      net_buf_add(packet, len);

      /* The server owns the packet from here: it reassembles the
       * byte stream, so how it is chunked here cannot matter.
       */
      int ret = _server.enqueue_packet(packet);

      if (ret < 0) {
        LOG_ERR("Feeding the HTTP server failed (%d)", ret);
        net_buf_unref(packet);
      }
    }
  }
}

int UartBridge::start() {
  if (!device_is_ready(_uart)) {
    LOG_ERR("UART %s is not ready", _uart->name);
    return -ENODEV;
  }

  ring_buf_init(&_rx_ring, sizeof(_rx_ring_buf), _rx_ring_buf);
  k_sem_init(&_rx_sem, 0, 1);

  /* The consumer must exist before the producer: start the feed thread
   * first, then let the ISR begin filling the ring.
   */
  k_thread_create(&_feed_thread, uart_feed_stack,
                  K_THREAD_STACK_SIZEOF(uart_feed_stack), feed_trampoline, this,
                  nullptr, nullptr, UART_FEED_PRIORITY, 0, K_NO_WAIT);
  (void)k_thread_name_set(&_feed_thread, "uart_feed");

  /* -ENOSYS/-ENOTSUP here means no interrupt-driven UART API: without
   * this check the bridge would come up silently deaf.
   */
  int ret = uart_irq_callback_user_data_set(_uart, isr_trampoline, this);
  if (ret < 0) {
    LOG_ERR("UART %s has no interrupt-driven API (%d)", _uart->name, ret);
    return ret;
  }

  uart_irq_rx_enable(_uart);

  LOG_INF("UART %s wired to the HTTP server", _uart->name);

  return 0;
}
