# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Round-trip gate for CAP_REPLY's packing in kernel/include/kickos/cap.h. The header is
# standalone (stdint.h, config/system.h, sys/cap_index.h), so a host program drives the real
# cap_reply_seq_seat / cap_reply_seq / cap_reply_handle. Host-only, like the slotpool gate: the
# case that matters is a thread index above 255, and no board configures that many slots.
add_executable(capreply_packing capreply_packing.cc)
target_include_directories(capreply_packing PRIVATE
  "${PROJECT_SOURCE_DIR}/kernel/include" "${PROJECT_SOURCE_DIR}/system/include")
target_compile_options(capreply_packing PRIVATE ${KICKOS_WARN_FLAGS})

add_test(NAME capreply_packing COMMAND "$<TARGET_FILE:capreply_packing>")
set_tests_properties(capreply_packing PROPERTIES TIMEOUT 30 LABELS host)
