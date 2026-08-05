# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The sample app consumes the library the same way any other application
# would: through the Zephyr module in this repository (zephyr/module.yml).
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(http_server_bare CXX)

target_sources(app PRIVATE
  src/main.cpp
  src/uart_bridge.cpp
)
