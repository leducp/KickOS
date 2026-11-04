# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Where the ROM boots esptool's image format, every image this configure emits holds at most the
# segments the ROM loader reads and loads no window (kickos-image-maps.txt lists them), read off
# its program headers, and where esptool wrote its .app.bin, the count esptool wrote agrees.

if(NOT DEFINED KICKOS_CHIP_ESPTOOL_IMAGE)
  return()
endif()
string(REPLACE ";" "," _esp_segments_options "${KICKOS_CHIP_ESPTOOL_IMAGE}")
set(_esp_segments_app_bin "")
if(KICKOS_ESPTOOL)
  set(_esp_segments_app_bin --app-bin)
endif()
add_test(NAME ${_tag}_images_esp_segments
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_esp_segments.sh"
          --images "${PROJECT_BINARY_DIR}/kickos-image-maps.txt" "${CMAKE_READELF}"
          "${CMAKE_NM}" "${_esp_segments_options}" ${_esp_segments_app_bin})
kickos_host_gate(${_tag}_images_esp_segments TIMEOUT 120)
