# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The consumer behind tests/integration/check_system_link.sh: an image built against an installed
# KickOS package that the link refuses, its arm KICKOS_PROBE_ARM:
#   none      KickOS::kernel and no system target;
#   two       KickOS::system_default and a second system target of KICKOS_PROBE_COMPOSITION;
#   composed  one system target of KICKOS_PROBE_COMPOSITION;
#   tls       the same, the image declaring a thread-local object;
#   noheap    the same, its link options, which define the heap, taken away;
#   capture   the same, the image holding one live object in each section KICKOS_PROBE_SECTIONS
#             names.

cmake_minimum_required(VERSION 3.24)
# KICKOS_PROBE_ARM nocc leaves C out, which kickos_compose refuses.
if(KICKOS_PROBE_ARM STREQUAL "nocc")
  project(kickos_system_link LANGUAGES CXX ASM)
  find_package(KickOS REQUIRED)
  kickos_compose(probe_system "${KICKOS_PROBE_COMPOSITION}")
  return()
endif()
project(kickos_system_link LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

set(_sources main.c)
if(KICKOS_PROBE_ARM STREQUAL "tls")
  list(APPEND _sources tls.c)
endif()
add_executable(probe ${_sources})
if(KICKOS_PROBE_ARM STREQUAL "tls")
  target_compile_definitions(probe PRIVATE KICKOS_PROBE_TLS=1)
endif()
if(KICKOS_PROBE_ARM STREQUAL "capture")
  if(NOT KICKOS_PROBE_SECTIONS)
    message(FATAL_ERROR "KICKOS_PROBE_ARM capture names no KICKOS_PROBE_SECTIONS")
  endif()
  set(_capture "int kickos_probe_capture(void);\n")
  set(_reads "")
  set(_i 0)
  foreach(_section IN LISTS KICKOS_PROBE_SECTIONS)
    string(APPEND _capture "__attribute__((section(\"${_section}\"), used)) "
                           "const unsigned kickos_probe_section_${_i} = ${_i}u;\n")
    string(APPEND _reads " + *(volatile const unsigned*)&kickos_probe_section_${_i}")
    math(EXPR _i "${_i} + 1")
  endforeach()
  string(APPEND _capture "int kickos_probe_capture(void)\n{\n    return 0${_reads};\n}\n")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/capture.c" "${_capture}")
  target_sources(probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/capture.c")
  target_compile_definitions(probe PRIVATE KICKOS_PROBE_CAPTURE=1)
endif()

if(KICKOS_PROBE_ARM STREQUAL "none")
  target_link_libraries(probe PRIVATE KickOS::kernel)
elseif(KICKOS_PROBE_ARM STREQUAL "two")
  kickos_compose(second_system "${KICKOS_PROBE_COMPOSITION}")
  target_link_libraries(probe PRIVATE KickOS::kickos KickOS::system_default second_system)
elseif(KICKOS_PROBE_ARM MATCHES "^(composed|tls|noheap|capture)$")
  kickos_compose(probe_system "${KICKOS_PROBE_COMPOSITION}")
  target_link_libraries(probe PRIVATE KickOS::kickos probe_system)
  if(KICKOS_PROBE_ARM STREQUAL "noheap")
    set_property(TARGET probe_system PROPERTY INTERFACE_LINK_OPTIONS "")
  endif()
else()
  message(FATAL_ERROR "KICKOS_PROBE_ARM is '${KICKOS_PROBE_ARM}', not none, two, composed, "
                      "tls, noheap or capture")
endif()
kickos_emit_image(probe)
