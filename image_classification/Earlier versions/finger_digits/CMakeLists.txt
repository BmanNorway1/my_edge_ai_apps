#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(finger_digits)

list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

target_sources(app PRIVATE
  src/main.c
  src/drivers/arducam_mega.c

)


