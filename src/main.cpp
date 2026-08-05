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

#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#include <raw_http_server.hpp>
#include <uart_bridge.hpp>

LOG_MODULE_REGISTER(http_server_bare, LOG_LEVEL_INF);

/*
 * Files are served from an already-mounted directory. A real board typically
 * mounts its storage during boot and simply hands the path to the server; this
 * sample mounts a littlefs on native_sim so it has something to serve.
 */
#define FILES_MOUNT_POINT "/lfs"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t files_mount = {
	.type = FS_LITTLEFS,
	.mnt_point = FILES_MOUNT_POINT,
	.fs_data = &storage,
	.storage_dev = (void *)PARTITION_ID(storage_partition),
};

/* A file to download, so the sample has something to serve out of the box. */
static int seed_demo_file(void)
{
	static const char body[] = "hello from the bare HTTP server\n";
	struct fs_file_t file;
	int ret;

	fs_file_t_init(&file);

	ret = fs_open(&file, FILES_MOUNT_POINT "/hello.txt", FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		return ret;
	}

	ret = fs_write(&file, body, sizeof(body) - 1);
	(void)fs_close(&file);

	return ret < 0 ? ret : 0;
}

/* The UART carrying HTTP. Constructing it does no work, so namespace scope is
 * safe; everything happens in start().
 */
static UartBridge bridge(DEVICE_DT_GET(DT_ALIAS(http_uart)), FILES_MOUNT_POINT);

#if defined(CONFIG_APP_SELFTEST)
/*
 * Inject a few requests directly, so the sample demonstrates itself without a
 * terminal attached. The responses go out of the UART like any other.
 */
static const char *const selftest_requests[] = {
	/* download the seeded file */
	"GET " CONFIG_RAW_HTTP_FILES_PREFIX "hello.txt HTTP/1.1\r\nHost: bare\r\n\r\n",
	/* upload a new one ... */
	"PUT " CONFIG_RAW_HTTP_FILES_PREFIX "upload.txt HTTP/1.1\r\nHost: bare\r\n"
	"Content-Length: 21\r\n\r\nuploaded over a UART\n",
	/* ... and read it back */
	"GET " CONFIG_RAW_HTTP_FILES_PREFIX "upload.txt HTTP/1.1\r\nHost: bare\r\n\r\n",
	/* a missing file 404s and the stream keeps serving */
	"GET " CONFIG_RAW_HTTP_FILES_PREFIX "missing.txt HTTP/1.1\r\nHost: bare\r\n\r\n",
};

static void run_selftest(RawHttpServer &server)
{
	ARRAY_FOR_EACH(selftest_requests, i) {
		const char *request = selftest_requests[i];
		size_t len = strlen(request);
		size_t first = len / 2;
		int ret;

		LOG_INF("--> request %zu: feeding %zu bytes in two chunks", i + 1, len);

		/* Split on purpose: the server does not care about buffer
		 * boundaries, only about the byte stream.
		 */
		ret = server.input(request, first);
		if (ret == 0) {
			ret = server.input(request + first, len - first);
		}

		if (ret < 0) {
			LOG_ERR("input failed (%d)", ret);
			return;
		}
	}
}
#endif /* CONFIG_APP_SELFTEST */

int main(void)
{
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
