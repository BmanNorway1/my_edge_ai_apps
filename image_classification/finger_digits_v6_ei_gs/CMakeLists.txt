#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

# NOTE: DTS_ROOT and EXTRA_CONF_FILE must be set BEFORE find_package(Zephyr),
# otherwise Zephyr never sees them (the conf overlay is silently ignored and
# custom devicetree bindings are not found).
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/ei-model/conf_overlay.conf)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(finger_digits_v3)

# Compile the Edge Impulse SDK shipped with THIS export directly into `app`
# (provides run_classifier(), the image DSP block and the Nordic Axon wrapper).
# IMPORTANT: keep CONFIG_EDGE_IMPULSE_SDK=n. The export's cmake takes its
# "integrate into app" branch only when that symbol is undefined; setting it =y
# would instead trigger the separate edge-impulse-sdk-zephyr *module* (a
# different SDK version that does not match this model's model_variables.h).
add_subdirectory(ei-model/edge-impulse-sdk/cmake/zephyr)

# Model headers + the NRF_AXON_MODEL_NAME macro the Axon wrapper token-pastes to
# locate the compiled model struct/header (value from conf_overlay.conf).
zephyr_include_directories(ei-model/nordic-axon-model)
target_include_directories(app PRIVATE ei-model)
zephyr_compile_definitions(NRF_AXON_MODEL_NAME=${CONFIG_NRF_AXON_MODEL_NAME})

target_sources(app PRIVATE
  src/main.cpp
  src/drivers/arducam_mega.c
)



