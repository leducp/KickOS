# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Round-trip gate for CAP_REPLY's packing in kernel/include/kickos/cap.h. The header is
# standalone (stdint.h, config/system.h, sys/cap_index.h), so a host program drives the real
# cap_reply_seq_seat / cap_reply_seq / cap_reply_handle. Host-only, like the slotpool gate: the
# case that matters is a thread index above 255, and no board configures that many slots.
kickos_add_unit_test(NAME capreply_packing
  SOURCES capreply_packing.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/include" "${PROJECT_SOURCE_DIR}/system/include")
