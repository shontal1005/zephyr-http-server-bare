# Copyright (c) 2026 Zephyr Project contributors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_RAW_HTTP_SERVER)
  zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../include)

  zephyr_library_named(raw_http_server)
  zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/../lib/raw_http_server.cpp)
  zephyr_library_sources_ifdef(CONFIG_RAW_HTTP_UART_BRIDGE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/uart_bridge.cpp)
endif()
