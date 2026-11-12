# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Console demo: the board's composition names its packaged IRQ UART driver as stdout, and main and
# a worker print through it. Judged from a silicon capture.
add_executable(uartconsole main.cc)
kickos_app_system(uartconsole COMPOSITION systems/${KICKOS_BOARD}.yaml)
kickos_emit_image(uartconsole)

# The judge takes the driver's up line as its source prints it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/systems/${KICKOS_BOARD}.yaml" _uc_driver
     REGEX "^ *driver: " LIMIT_COUNT 1)
string(REGEX REPLACE "^ *driver: *([a-z0-9_]+).*$" "\\1" _uc_driver "${_uc_driver}")
file(GLOB _uc_sources "${PROJECT_SOURCE_DIR}/system/driver/*/${_uc_driver}/*.cc")
set(_uc_up "")
foreach(_uc_source IN LISTS _uc_sources)
  file(STRINGS "${_uc_source}" _uc_line REGEX "\"\\[${_uc_driver}\\] [^\"]* up \\(" LIMIT_COUNT 1)
  if(_uc_line)
    string(REGEX REPLACE "^.*\"(\\[${_uc_driver}\\] [^\"]* up \\([^\"]*)\\\\n\".*$" "\\1" _uc_up
           "${_uc_line}")
  endif()
endforeach()
if(_uc_up STREQUAL "")
  message(FATAL_ERROR "uartconsole: ${_uc_driver}, the driver systems/${KICKOS_BOARD}.yaml names, "
                      "prints no up line in its source")
endif()
kickos_app_judge(uartconsole tests/integration/check_uartconsole.sh ARGS "${_uc_up}")

# Scramble-then-panic reclaim mode: an unprivileged thread holding no grant garbles the K64's UART0
# through the slot the driver opened and panics, and the panic must still reach the wire, reclaimed
# out of the published console. Always defined: the app is built -Wundef -Werror.
option(UARTCONSOLE_SCRAMBLE_TEST "uartconsole: scramble-then-panic console-reclaim test mode (K64F)"
       OFF)
if(UARTCONSOLE_SCRAMBLE_TEST)
  if(NOT KICKOS_CHIP STREQUAL "mk64f")
    message(FATAL_ERROR "UARTCONSOLE_SCRAMBLE_TEST garbles the K64's UART0, and this chip is "
                        "${KICKOS_CHIP}")
  endif()
  target_compile_definitions(uartconsole PRIVATE UARTCONSOLE_SCRAMBLE_TEST=1)
else()
  target_compile_definitions(uartconsole PRIVATE UARTCONSOLE_SCRAMBLE_TEST=0)
endif()
