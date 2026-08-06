/*
 * Copyright (c) 2026 Zephyr Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Serving files over HTTP with bare data buffers.
 *
 * There is no network stack in this image: no L2 driver, no IP, no TCP, no
 * sockets. A UART is wired straight into RawHttpServer, so bytes arriving on
 * the wire become HTTP requests and the responses go back out of the same
 * UART. GET downloads a file, PUT and POST upload one.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/storage/flash_map.h>

#include <raw_http_server.hpp>

#include "uart_bridge.hpp"

LOG_MODULE_REGISTER(http_server_bare, LOG_LEVEL_INF);

/*
 * The URL is the filesystem path, so whatever is mounted is servable at its
 * mount point: this littlefs at /lfs makes "GET /lfs/hello.txt" work. A real
 * board typically mounts its storage during boot and mounts nothing here.
 */
#define FILES_MOUNT_POINT "/lfs"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t files_mount = {
    .type = FS_LITTLEFS,
    .mnt_point = FILES_MOUNT_POINT,
    .fs_data = &storage,
    .storage_dev = (void*)PARTITION_ID(storage_partition),
};

/* A file to download, so the sample has something to serve out of the box. */
static int seed_demo_file(void) {
  static const char body[] = "hello from the bare HTTP server\n";
  struct fs_file_t file;
  int ret;

  fs_file_t_init(&file);

  ret =
      fs_open(&file, FILES_MOUNT_POINT "/hello.txt", FS_O_CREATE | FS_O_WRITE);
  if (ret < 0) {
    return ret;
  }

  ret = fs_write(&file, body, sizeof(body) - 1);
  (void)fs_close(&file);

  /* A short write (e.g. no free space) would silently truncate the demo. */
  if (ret >= 0 && ret != (int)(sizeof(body) - 1)) {
    return -EIO;
  }

  return ret < 0 ? ret : 0;
}

/* Packet size only sets how RX bytes are chunked on the way to the server. */
#define HTTP_PACKET_SIZE 1024
#define HTTP_PACKET_COUNT 8

NET_BUF_POOL_DEFINE(http_packet_pool,
                    HTTP_PACKET_COUNT,
                    HTTP_PACKET_SIZE,
                    0,
                    NULL);

/* The UART carrying HTTP. Constructing the bridge starts the server's thread,
 * which is safe at namespace scope; the UART side happens in start().
 */
static UartBridge bridge(DEVICE_DT_GET(DT_ALIAS(http_uart)), &http_packet_pool);

#if defined(CONFIG_APP_SELFTEST)
/*
 * Inject a few requests directly, so the sample demonstrates itself without a
 * terminal attached. The responses go out of the UART like any other.
 */
static const char* const selftest_requests[] = {
    /* download the seeded file */
    "GET " FILES_MOUNT_POINT "/hello.txt HTTP/1.1\r\nHost: bare\r\n\r\n",
    /* upload a new one ... */
    "PUT " FILES_MOUNT_POINT
    "/upload.txt HTTP/1.1\r\nHost: bare\r\n"
    "Content-Length: 21\r\n\r\nuploaded over a UART\n",
    /* ... and read it back */
    "GET " FILES_MOUNT_POINT "/upload.txt HTTP/1.1\r\nHost: bare\r\n\r\n",
    /* a missing file 404s and the stream keeps serving */
    "GET " FILES_MOUNT_POINT "/missing.txt HTTP/1.1\r\nHost: bare\r\n\r\n",
};

/* Wrap a slice of a request in a packet and queue it for the server. */
static int enqueue_selftest_bytes(RawHttpServer& server,
                                  const char* bytes,
                                  size_t len) {
  struct net_buf* packet = net_buf_alloc(&http_packet_pool, K_FOREVER);
  int ret;

  net_buf_add_mem(packet, bytes, len);

  ret = server.enqueue_packet(packet);
  if (ret < 0) {
    net_buf_unref(packet);
  }

  return ret;
}

static void run_selftest(RawHttpServer& server) {
  ARRAY_FOR_EACH(selftest_requests, i) {
    const char* request = selftest_requests[i];
    size_t len = strlen(request);
    size_t first = len / 2;
    int ret;

    LOG_INF("--> request %zu: feeding %zu bytes in two packets", i + 1, len);

    /* Split on purpose: the server does not care about packet
     * boundaries, only about the byte stream. The responses come
     * out of the UART asynchronously, from the server's thread.
     */
    ret = enqueue_selftest_bytes(server, request, first);
    if (ret == 0) {
      ret = enqueue_selftest_bytes(server, request + first, len - first);
    }

    if (ret < 0) {
      LOG_ERR("enqueue_packet failed (%d)", ret);
      return;
    }
  }
}
#endif /* CONFIG_APP_SELFTEST */

int main(void) {
  int ret;

  /* A real board would have done this during boot; the server only ever
   * receives the resulting path.
   */
  ret = fs_mount(&files_mount);
  if (ret < 0) {
    LOG_ERR("Cannot mount %s (%d)", FILES_MOUNT_POINT, ret);
    return ret;
  }

  ret = seed_demo_file();
  if (ret < 0) {
    LOG_WRN("Cannot seed the demo file (%d)", ret);
  }

  LOG_INF("Serving files from %s", FILES_MOUNT_POINT);

  ret = bridge.start();
  if (ret < 0) {
    LOG_ERR("Cannot wire up the UART (%d)", ret);
    return ret;
  }

#if defined(CONFIG_APP_SELFTEST)
  run_selftest(bridge.server());
  LOG_INF("Self-test done, now serving the UART forever");
#endif

  /* Everything from here on is driven by the UART interrupt. */
  return 0;
}
