# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Contract gate for the raw UART class <kickos/driver/uart.h>, with uart_mock.cc as a second
# implementation of the five public class symbols.
#
# HOST-ONLY: a target image carrying this mock would define kos_uart_* on its link line, and
# the real backend's archive member would then never be extracted, with no duplicate symbol
# reported (tests/static/check_class_backend.sh).
kickos_add_unit_test(NAME uart_class
  SOURCES uart_class.cc uart_mock.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include")
