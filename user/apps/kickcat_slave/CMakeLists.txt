# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Stage A (docs/design-kickcat-k64f.md): a KickCAT EtherCAT slave on the KickOS
# sim over the software EmulatedESC. KickCAT is used, not vendored -- the sibling
# checkout is brought in with the KickOS OS backend selected (KICKOS=ON), and the
# app links the plain `kickcat` target. Sim-only; self-skips if KickCAT is absent.

set(KICKCAT_DIR "${CMAKE_SOURCE_DIR}/../KickCAT"
    CACHE PATH "Path to the sibling KickCAT checkout")

if(NOT EXISTS "${KICKCAT_DIR}/lib/slave/include/kickcat/ESC/EmulatedESC.h")
  message(STATUS "KickOS: KickCAT not found at ${KICKCAT_DIR} -- skipping kickcat_slave")
  return()
endif()

# Select the KickOS OS backend inside KickCAT (Time-only; no socket/pthreads) and
# build it as an ordinary host static lib (full C++: exceptions/RTTI/STL/heap on).
set(KICKOS ON)
add_subdirectory("${KICKCAT_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/kickcat" EXCLUDE_FROM_ALL)

kickos_add_application(kickcat_slave
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
target_link_libraries(kickcat_slave PRIVATE kickcat)

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME kickcat_slave COMMAND "$<TARGET_FILE:kickcat_slave>")
  set_tests_properties(kickcat_slave PROPERTIES
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION "slave reached PRE_OP")
endif()
