# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Phenix-1255 contributors

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(phenix_1255_firmware LANGUAGES C)

# Module headers are included with their folder, e.g. "stream/stream.h"
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
)

target_sources(app PRIVATE
  src/main.c
  src/board/board_io.c
  src/board/watchdog.c
  src/config/config_store.c
  src/control/control.c
  src/display/display_fb.c
  src/display/display_ui.c
  src/ethernet/net_config.c
  src/ethernet/transport_eth.c
  src/ethernet/web_ui.c
  src/radio/sx1255.c
  src/shell/shell_commands.c
  src/stream/sample_io.c
  src/stream/stream.c
  src/usb/transport_usb.c
)

# SRAM A..H holds the DMA buffers (SAI, Ethernet, USB) and the network
# stack. Buffers and thread stacks that only the CPU accesses are moved to
# SRAMX (96 KB, otherwise unused) to make room for the USB stack. None of
# these files may contain DMA buffers.
# The relocation script finds the object by file name and parent folder name
# and may pick a Zephyr object instead (src/shell/shell_cmds.c matched
# subsys/shell/shell_cmds.c), so file names here must not exist in Zephyr.
zephyr_code_relocate(
  FILES
    src/display/display_fb.c
    src/display/display_ui.c
    src/ethernet/transport_eth.c
    src/ethernet/web_ui.c
    src/shell/shell_commands.c
    src/stream/sample_io.c
  LOCATION SRAMX_BSS_NOINIT
)

# SAI and eDMA are driven directly by the stream module. The MCUX HAL only
# builds these drivers together with the Zephyr I2S/DMA drivers, which are
# not used here, so the HAL sources are added to the application.
set(MCUX_DRIVERS ${ZEPHYR_HAL_NXP_MODULE_DIR}/mcux/mcux-sdk-ng/drivers)
target_include_directories(app PRIVATE
  ${MCUX_DRIVERS}/sai
  ${MCUX_DRIVERS}/edma4
)
target_sources(app PRIVATE
  ${MCUX_DRIVERS}/sai/fsl_sai.c
  ${MCUX_DRIVERS}/edma4/fsl_edma.c
)
