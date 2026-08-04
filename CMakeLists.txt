# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(http_server_bare)

target_sources(app PRIVATE
  src/main.c
  src/bare_transport.c
)

zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(
  NAME http_resource_desc_bare_service
  KVMA RAM_REGION GROUP RODATA_REGION
)
