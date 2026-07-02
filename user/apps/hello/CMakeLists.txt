# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The friendly demo: the same 3-line consumer shape a downstream project uses.
kickos_add_application(hello
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# Light smoke test: runs the endless demo briefly, Ctrl+C, checks it ping-ponged
# and halted cleanly.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    hello_demo
    COMMAND "${CMAKE_COMMAND}" -E env
            "${PROJECT_SOURCE_DIR}/tests/check_hello.py"
            "$<TARGET_FILE:hello>")
  set_tests_properties(hello_demo PROPERTIES TIMEOUT 15)
endif()
