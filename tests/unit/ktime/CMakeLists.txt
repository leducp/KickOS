# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Deadline-stability gate for kernel/time/time.cc. Compiles the REAL time.cc into a
# host program against a fake clock and a recording arch_timer_arm, so the gate reads
# the exact value the arch backends dedup on, a quantity no on-target arm can see.
#
# Takes the kernel target's include path and definitions verbatim: this TU compiles a
# kernel source, so a divergent config here would gate a different translation unit
# than the one that ships.
#
# The deadline ktime_rearm hands arch_timer_arm must be STABLE across repeated calls:
# it runs on every context switch, and re-deriving it from the clock defeats every
# backend's dedup (g_armed_deadline_ns on ARM, g_rx_armed_ns on RX, the absolute compare
# on CLINT/POSIX), so the one-shot restarts from zero before reaching its compare and a
# deadline inside the min-delta window starves. Mutation-proved: restoring the floor to
# ktime_rearm makes this print the drift directly (1017500, 1020000, 1022500 ... where
# 1015000 is wanted) and fail 11 ways.
kickos_add_unit_test(NAME ktime_rearm
  SOURCES ktime_rearm.cc "${PROJECT_SOURCE_DIR}/kernel/time/time.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
