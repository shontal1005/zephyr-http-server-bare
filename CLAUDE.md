# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A GET-only HTTP/1.1 file server for Zephyr that runs on bare data buffers — no sockets, L2, IP or TCP in the image. `RawHttpServer` drives Zephyr's bundled nodejs `http_parser` directly; the only networking code built is that parser library. The URL **is** the filesystem path (a mount at `/lfs` serves `GET /lfs/...`): GET downloads, any other method → 405. Packets carry arbitrary chunks of the request byte stream — the server does the framing: a request may span any number of packets, one packet may carry several pipelined requests, and each is answered in order as its head completes.

## Build, run, test

The Zephyr tree at `../zephyr` (v4.4.0) is standalone — **`west build` does not work**; drive CMake directly. Module dependencies come from the west workspace at `../workspace` (plus `../zephyr44-modules/` for HALs re-pinned to match v4.4).

```sh
# native_sim (host toolchain)
ZEPHYR_BASE=/home/user/dev/zephyr ZEPHYR_TOOLCHAIN_VARIANT=host \
  cmake -B build -GNinja -DBOARD=native_sim \
  -DZEPHYR_MODULES=/home/user/dev/workspace/modules/fs/littlefs .
ninja -C build

# run the simulator; it prints "uart_1 connected to pseudotty: /dev/pts/N"
# (it persists its flash image as ./flash.bin in the cwd — gitignored)
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
python3 -m pytest tests/test_raw_http_client.py::test_download_spans_several_chunks  # one test
tests/http_over_serial_test.py /dev/pts/N                  # standalone script, same checks
```

Use `--device=...` with `=`: a bare path value derails pytest's rootdir detection (pytest.ini pins it, but keep the habit). The firmware's boot self-test pushes five responses out the HTTP UART — clients must drain before their first request (`RawHttpClient.drain()` / the scripts do this).

## Architecture

**The repo is both a Zephyr module and a sample app consuming it.**

- Library = `RawHttpServer` alone: `include/raw_http_server.hpp` + `lib/raw_http_server.cpp`, exposed via `zephyr/module.yml`, `zephyr/CMakeLists.txt`, `zephyr/Kconfig` (`CONFIG_RAW_HTTP_SERVER`). The root `CMakeLists.txt` consumes the module through `ZEPHYR_EXTRA_MODULES` pointing at the repo itself — the same way any external app would.
- Sample app = everything under `src/`: `main.cpp` (littlefs mount, seeds `hello.txt` and the 5035-byte `big.bin` — pattern `(i*7 + i//251) & 0xFF`, mirrored by the tests' `payload_of()`; it exceeds `CONFIG_RAW_HTTP_FILE_CHUNK` so multi-chunk downloads are testable — plus the boot self-test) and `uart_bridge.{hpp,cpp}` (deliberately **not** part of the library; its knobs are plain `#define`s).
- Host side = `client/raw_http_client.py`, the Python mirror: `RawHttpClient` is transport-blind (override `send()`/`receive()`); `download()` is the real surface, `request(raw)` returns `(status, body)` so tests can probe refusal statuses; responses are read from a persistent `_rx` buffer so pipelined responses spanning one `receive()` aren't lost (`drain()` clears it); `SerialRawHttpClient` is the UART implementation; the file doubles as a CLI (`download` subcommand only). `tests/` drive it — 25 pytest cases: splits at arbitrary/every-interesting boundary, byte-at-a-time dribble, pipelining in one write, a pipelined batch split mid-request, late-arriving refused bodies, parametrized methods, CONNECT/upgrade, 414/431/garbage, a marathon mix.

**Data flow on target:** UART ISR → ring buffer → feed thread (a plain chunker: wraps whatever the ring holds in `net_buf` packets from the pool in `main.cpp` as bytes arrive — no framing, packet boundaries cannot matter) → `RawHttpServer::enqueue_packet()` → k_fifo → the server's own thread → `handle_packet()` interleaves three consumers over each packet: bytes owed to an answered request's body are discarded first (`_skip`, counted down by `Content-Length` — bodies are NEVER buffered), the rest is appended to `_request_buf` (`CONFIG_RAW_HTTP_HEAD_MAX` bytes; a head that outgrows a full buffer → 431 + buffer reset), and `process_buffer()` serves every head that completes — re-parsing the buffer from the start with a freshly `http_parser_init`ed parser on each attempt, so no parser state spans packets. Two callbacks: `on_url` accumulates the possibly-split URL, failing the parse on overflow; `on_headers_complete` deliberately returns -1 to halt the parser at the end of the head (its 0/1/2 returns are magic to the parser; -1 is a plain error — and a GET-only server never needs the body parsed). The parser's errno is the entire verdict: `HPE_CB_headers_complete` is the SUCCESS code, `HPE_OK` = head incomplete → WAIT silently for more bytes (not an error), `HPE_CB_url` = URL outgrew `CONFIG_RAW_HTTP_URL_MAX` → 414, anything else = malformed bytes → 400 + buffer reset. After a complete head: upgrade/CONNECT → 400 + reset (what follows isn't HTTP), chunked body → 400 + reset (no predeclared length to skip), non-GET → 405 as soon as the head completes (body then discarded via `_skip` as it streams past — even arriving after the 405), otherwise the query string is cut and the URL goes to the VFS (`fs_stat` before open so `Content-Length` leads; stat/open failure or a directory → 404; 200 streams the file) → `uart_poll_out()`. Responses — heads and download chunks alike — stage in the internal `_out_buf` of `CONFIG_RAW_HTTP_FILE_CHUNK` bytes (min 96, the worst-case response head — `response_head_max` in the `.cpp`). The class owns one thread and **no mutex**: the thread is the only toucher of request state, `enqueue_packet()` (ISR-safe) only touches the fifo, and single-consumer ordering serialises requests — a packet arriving mid-response waits in the fifo. Responses always carry `Content-Length` (never chunked). There is no connection object — keep-alive falls out of the design. The ISR/feed-thread split exists because `net_buf_alloc(K_FOREVER)` may block on an exhausted pool, which the ISR may not; the ring absorbs bytes meanwhile. `uart_poll_out` blocking on the server thread is the TX flow control — the server can't outrun the wire.

**The Kconfig trick (why networking symbols appear at all):** `RAW_HTTP_SERVER` *selects* `NETWORKING` solely because the parser's Kconfig lives under that menu — `select` forces only that symbol. The module Kconfig then flips `NET_NATIVE` to `default n` (module Kconfigs parse before the subsystem tree; first satisfied default wins), so consumers get no IP stack by default. Two traps learned the hard way: board `Kconfig.defconfig` files parse *before* modules and outrank them (hence the explicit `CONFIG_NET_L2_ETHERNET=n` in `prj.conf` for native_sim), and `RAW_HTTP_SERVER` must depend on nothing — `depends on CPP` or `FILE_SYSTEM` creates Kconfig dependency loops through OpenThread's select chains, so it selects all its code deps instead.

**Wire behavior** (what the tests assert): packets are arbitrary chunks of the byte stream — requests may be split at any boundary, dribbled a byte at a time, or pipelined several to a packet — and every request gets exactly one response, **per request, not per packet**, in order. GET only, so statuses are 200/400/404/405/414/431 (500 exists only as `reason_of()`'s fallback), via Zephyr's `enum http_status` names. The URL, query string cut off, is handed to the VFS as the filesystem path — Zephyr's mount table is the validator, so anything the fs refuses (no mount there, `..` above a filesystem root, a directory, a too-long name) is a 404 and there is no path-building code at all. Non-GET → 405 (its body skipped by `Content-Length`); upgrade/CONNECT → 400; chunked body → 400; overlong URL → 414; head over `CONFIG_RAW_HTTP_HEAD_MAX` → 431; `Expect: 100-continue` is not implemented (a non-GET gets its 405 as soon as the head completes, before any body). An incomplete request gets NO response — silence until its bytes arrive, like a normal HTTP server. Malformed bytes → a single 400 + buffer reset; the stream self-heals at the next parseable request, though the refused request's own tail may earn one follow-up 400 (the 414/431/garbage tests drain before probing health). A zero-length packet is dropped silently. The bridge's RX-ring overflow just drops bytes loudly — the mangled request 400s itself and the stream heals at the next boundary. `enqueue_packet` rejects net_buf fragment chains with `-EINVAL` (caller keeps ownership on error); any packet size is fine. The class has a best-effort destructor (aborts the thread, drains the fifo). Responses may arrive over any number of output-callback invocations — never assume framing on the way out. A pty has no baud rate, so hosts must pace writes (the client's `pace`/`gap`, default 256 B / 5 ms) or the RX ring overflows; mid-request pauses are otherwise harmless. The self-test's five requests demonstrate 200 (seeded hello.txt), 404 (missing), 405 (PUT with body, skipped), 400 (`"this is not HTTP\r\n\r\n"`), and a GET split across two packets → 200 (reassembly) — clients drain five responses before their first request.

## Conventions

- All C++ is formatted with `clang-format --style=chromium` (a PostToolUse hook in `.claude/settings.json` formats edited C++ files automatically; Python is exempt).
- snake_case for methods and variables; private members `_snake_case` (leading underscore); PascalCase only for classes/type aliases.
- Doxygen on all methods in headers, private ones included; `//` comments in `.cpp` bodies (license headers stay `/* */`); comments stay short and explain only what the code can't.
- Magic numbers get a named constant with a one-line derivation in the `.cpp` (see `response_head_max` in `lib/raw_http_server.cpp`); promote to the header only if callers need it.
- Tunables are Kconfig options (`CONFIG_RAW_HTTP_*`) for the library, plain defines for the sample bridge.
- README.md is the project-facing doc; README.rst is the upstream-Zephyr-style twin — keep both in sync when behavior changes.
