# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Deadline-stability gate for kernel/time/time.cc. Compiles the REAL time.cc into a
# host program against a fake clock and a recording arch_timer_arm, so the gate reads the
# exact value handed to the backend, a quantity no on-target arm can see.
#
# Takes the kernel target's include path and definitions verbatim: this TU compiles a
# kernel source, so a divergent config here would gate a different translation unit
# than the one that ships.
#
# Whether ktime_rearm reprograms the comparator must not depend on the clock: it runs on every
# context switch, and a dedup key re-derived from the clock restarts the one-shot from zero
# before it reaches its compare, so a deadline inside the min-delta window starves. What it
# programs, once it does, is never closer than the min delta to the clock it reads then.
kickos_add_unit_test(NAME ktime_rearm
  SOURCES ktime_rearm.cc "${PROJECT_SOURCE_DIR}/kernel/time/time.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
# Again for an arch whose switcher exception lands a booked switch later.
kickos_add_unit_test(NAME ktime_rearm_late
  SOURCES ktime_rearm.cc "${PROJECT_SOURCE_DIR}/kernel/time/time.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>
  POSTURE KICKOS_ARCH_SWITCH_LANDS_LATER=1)

# The wall clock one partition keeps across its nodes, over the REAL kernel/time/realtime.cc: two
# nodes as two instances of one program, and a node's set stopped inside its writes.
kickos_add_unit_test(NAME wall_shared
  SOURCES wall_shared.cc "${PROJECT_SOURCE_DIR}/kernel/time/realtime.cc"
  POSTURE KICKOS_AMP_CLOCK_SHARED=1 KICKOS_AMP_NODES=2 KICKOS_MULTI_INSTANCE=1
          KICKOS_MAX_INSTANCES=2)

# Per-core delta lists at two kernel cores: which core arms for, expires and re-arms after a
# deadline when its park, its removal and a set of the wall clock land on different cores.
# Host-only: the cores' timers and dispatches are sequenced here, which no image can arrange.
#
# Needs kickos_kseam_time_smp (tests/unit/semtimed) to exist before this directory is added.
kickos_add_unit_test(NAME sleepq_cores SOURCES sleepq_cores.cc
  LIBRARIES kickos_kseam_time_smp
  TEST_PREFIX smp.)
