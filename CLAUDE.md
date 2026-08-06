# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An HTTP/1.1 file server for Zephyr that runs on bare data buffers — no sockets, L2, IP or TCP in the image. `RawHttpServer` drives Zephyr's bundled nodejs `http_parser` directly; the only networking code built is that parser library. GET downloads a file from an already-mounted directory, PUT/POST upload one into it.

## Build, run, test

The Zephyr tree at `../zephyr` (v4.4.0) is standalone — **`west build` does not work**; drive CMake directly. Module dependencies come from the west workspace at `../workspace` (plus `../zephyr44-modules/` for HALs re-pinned to match v4.4).

```sh
# native_sim (host toolchain)
ZEPHYR_BASE=/home/user/dev/zephyr ZEPHYR_TOOLCHAIN_VARIANT=host \
  cmake -B build -GNinja -DBOARD=native_sim \
  -DZEPHYR_MODULES=/home/user/dev/workspace/modules/fs/littlefs .
ninja -C build

# run the simulator; it prints "uart_1 connected to pseudotty: /dev/pts/N"
build/zephyr/zephyr.exe

# nucleo_u5a5zj_q (Zephyr SDK found via the CMake package registry)
ZEPHYR_BASE=/home/user/dev/zephyr cmake -B build_u5 -GNinja -DBOARD=nucleo_u5a5zj_q \
  -DZEPHYR_MODULES="/home/user/dev/workspace/modules/fs/littlefs;/home/user/dev/zephyr44-modules/hal_stm32;/home/user/dev/workspace/modules/hal/cmsis;/home/user/dev/workspace/modules/hal/cmsis_6"
ninja -C build_u5

# flash the Nucleo (ST-Link; console on /dev/ttyACM0, HTTP UART on the FTDI /dev/ttyUSB0)
~/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
  -c port=SWD mode=UR -w build_u5/zephyr/zephyr.hex -v -rst
```

Tests (pytest drives the Python client end to end; with no options the conftest launches `build/zephyr/zephyr.exe` itself and reads the pty):

```sh
python3 -m pytest tests/                                   # against native_sim
python3 -m pytest tests/ --device=/dev/ttyUSB0 --baud=115200   # against a real board
python3 -m pytest tests/test_raw_http_client.py::test_shorter_reupload_truncates  # one test
tests/http_over_serial_test.py /dev/pts/N                  # standalone script, same checks
```

Use `--device=...` with `=`: a bare path value derails pytest's rootdir detection (pytest.ini pins it, but keep the habit). The firmware's boot self-test pushes four responses out the HTTP UART — clients must drain before their first request (`RawHttpClient.drain()` / the scripts do this).

## Architecture

**The repo is both a Zephyr module and a sample app consuming it.**

- Library = `RawHttpServer` alone: `include/raw_http_server.hpp` + `lib/raw_http_server.cpp`, exposed via `zephyr/module.yml`, `zephyr/CMakeLists.txt`, `zephyr/Kconfig` (`CONFIG_RAW_HTTP_SERVER`). The root `CMakeLists.txt` consumes the module through `ZEPHYR_EXTRA_MODULES` pointing at the repo itself — the same way any external app would.
- Sample app = everything under `src/`: `main.cpp` (littlefs mount, seeds `hello.txt`, boot self-test) and `uart_bridge.{hpp,cpp}` (deliberately **not** part of the library; its knobs are plain `#define`s).
- Host side = `client/raw_http_client.py`, the Python mirror: `RawHttpClient` is transport-blind (override `send()`/`receive()`); `SerialRawHttpClient` is the UART implementation; the file doubles as a CLI. `tests/` drive it.

**Data flow on target:** UART ISR → ring buffer → feed thread (wraps bytes in `net_buf` packets from a pool defined in `main.cpp`) → `RawHttpServer::enqueue_packet()` → k_fifo → the server's own thread → `http_parser_execute()` fires five callbacks (`on_message_begin` marks a request in flight, `on_url` accumulates, `on_headers_complete` refuses upgrades with 400 and opens the upload file, `on_body` streams fragments to disk zero-copy, `on_message_complete` answers **inline** from the internal `_out_buf` staging buffer of `CONFIG_RAW_HTTP_FILE_CHUNK` bytes — never from the request's packet, so answering while the parser is still reading it is safe) → `uart_poll_out()`. Because each completed message is answered as the parser reaches it, one packet may carry several pipelined requests, each answered in order. The class owns one thread and **no mutex**: the thread is the only toucher of request state, `enqueue_packet()` (ISR-safe) only touches the fifo, and single-consumer ordering serialises requests — a packet arriving mid-response waits in the fifo. Responses always carry `Content-Length` (never chunked). There is no connection object — keep-alive falls out of the design; a malformed request gets a 400 and a parser reset, losing nothing else. The ISR/feed-thread split exists because `net_buf_alloc(K_FOREVER)` may block on an exhausted pool, which the ISR may not; the ring absorbs bytes meanwhile. `uart_poll_out` blocking on the server thread is the TX flow control — the server can't outrun the wire.

**The Kconfig trick (why networking symbols appear at all):** `RAW_HTTP_SERVER` *selects* `NETWORKING` solely because the parser's Kconfig lives under that menu — `select` forces only that symbol. The module Kconfig then flips `NET_NATIVE` to `default n` (module Kconfigs parse before the subsystem tree; first satisfied default wins), so consumers get no IP stack by default. Two traps learned the hard way: board `Kconfig.defconfig` files parse *before* modules and outrank them (hence the explicit `CONFIG_NET_L2_ETHERNET=n` in `prj.conf` for native_sim), and `RAW_HTTP_SERVER` must depend on nothing — `depends on CPP` or `FILE_SYSTEM` creates Kconfig dependency loops through OpenThread's select chains, so it selects all its code deps instead.

**Wire behavior** (what the tests assert): files served under `CONFIG_RAW_HTTP_FILES_PREFIX` (default `/files/`); statuses 200/201/400/404/405/414/500 via Zephyr's `enum http_status` names; `..` in names rejected; uploads open with `FS_O_TRUNC` so a shorter re-upload drops the old tail; `Expect: 100-continue` is not implemented. Malformed input gets a **single** 400 per failure burst (`_bad_stream` suppresses the rest until a request completes cleanly — one response per client round trip, never a 400 storm); upgrade/CONNECT → 400 (recorded in `on_headers_complete`; `process_packet`'s post-execute `_parser.upgrade` branch drops any upgraded-protocol remainder and reinits the parser to clear the sticky upgrade flag — not a stream failure). One packet may carry several pipelined requests — each is answered in order. An aborted upload deletes its partial file (`abort_upload()`), and a failed `fs_close` on upload → 500, not 201. A zero-length packet is the in-band stream-break marker: the bridge sends one after an RX-ring overflow, and the server fails any in-flight request with a 400 and resets the parser. `enqueue_packet` rejects net_buf fragment chains with `-EINVAL` (caller keeps ownership on error). The class has a best-effort destructor (aborts the thread, drains the fifo). Requests/responses may be split across any number of packet/callback invocations — never assume framing. A pty has no baud rate, so hosts must pace writes (the client's `pace`/`gap`, default 256 B / 5 ms) or the RX ring overflows.

## Conventions

- All C++ is formatted with `clang-format --style=chromium` (a PostToolUse hook in `.claude/settings.json` formats edited C++ files automatically; Python is exempt).
- snake_case for methods and variables; private members `_snake_case` (leading underscore); PascalCase only for classes/type aliases.
- Doxygen on all methods in headers, private ones included; `//` comments in `.cpp` bodies (license headers stay `/* */`); comments stay short and explain only what the code can't.
- Magic numbers get a named constant with a one-line derivation, in the `.cpp` unless truly public API (see `response_head_max` in `lib/raw_http_server.cpp` for the standard in-`.cpp` case).
- Tunables are Kconfig options (`CONFIG_RAW_HTTP_*`) for the library, plain defines for the sample bridge.
- README.md is the project-facing doc; README.rst is the upstream-Zephyr-style twin — keep both in sync when behavior changes.
