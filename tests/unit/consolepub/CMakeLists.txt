# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kos_console_publish's capability rule, kernel/syscall/cap.cc's cap_console_publish_through,
# against the real endpoint counters. Host-only: the EOVERFLOW arm needs an endpoint whose
# receiver count is already at its ceiling, which no image reaches.
kickos_add_unit_test(NAME console_publish_through SOURCES publish_through.cc
  LIBRARIES kickos_kseam)
