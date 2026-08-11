# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Contract gate for the raw UART class <kickos/driver/uart.h>, with uart_mock.cc as a second
# implementation of the five public class symbols.
#
# HOST-ONLY: a target image carrying this mock would define kos_uart_* on its link line, and
# the real backend's archive member would then never be extracted, with no duplicate symbol
# reported (tests/check_class_backend.sh).
add_executable(uart_class uart_class.cc uart_mock.cc)
target_include_directories(uart_class PRIVATE
  "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include")
target_compile_options(uart_class PRIVATE ${KICKOS_WARN_FLAGS})
add_test(NAME uart_class COMMAND "$<TARGET_FILE:uart_class>")
set_tests_properties(uart_class PROPERTIES TIMEOUT 30)
