# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The cfg refusals of the raw UART class <kickos/driver/uart.h>, its header inlines.
kickos_add_unit_test(NAME uart_class
  SOURCES uart_class.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include")
