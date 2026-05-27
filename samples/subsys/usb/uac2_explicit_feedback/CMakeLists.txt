# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(usb_audio_async_i2s)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
target_sources(app PRIVATE src/main.c)

if(CONFIG_SOC_COMPATIBLE_NRF5340_CPUAPP OR CONFIG_SOC_SERIES_NRF54H)
  target_sources(app PRIVATE src/feedback_nrf.c)
else()
  target_sources(app PRIVATE src/feedback_dummy.c)
endif()

## Cirrus Logic
if(CONFIG_HW_CODEC_CIRRUS_LOGIC)
  if(ZEPHYR_CIRRUS_LOGIC_MODULE_DIR)
    message(STATUS "Building with Cirrus Logic CS47L63 hardware codec support %s" ${ZEPHYR_CIRRUS_LOGIC_MODULE_DIR})

    # Include application events and configuration headers
    zephyr_library_include_directories(app PRIVATE
        ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf_audio/src/drivers
        ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf_audio/src/utils/macros
    )

    target_sources(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf_audio/src/drivers/cs47l63_comm.c)
    add_subdirectory(${ZEPHYR_CIRRUS_LOGIC_MODULE_DIR} cirrus_logic_bin_dir)
  else()
    message(FATAL_ERROR "Cirrus Logic/sdk-mcu-drivers repository not found\n")
  endif()
endif()
