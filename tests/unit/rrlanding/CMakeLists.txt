# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Round-robin rotations whose timer handler and switch each outlast the min delta, on the host
# fixture with the real time.cc: every thread rotated in must run before its next expiry. Once
# for an arch whose switcher exception lands the switch later, once for one that lands it at the
# interrupt's own exit.
set(_time "${PROJECT_SOURCE_DIR}/kernel/time/time.cc" "${PROJECT_SOURCE_DIR}/kernel/time/realtime.cc")
kickos_add_kseam(kickos_kseam_landing_late EXTRA ${_time}
  POSTURE KICKOS_HAVE_MPU=1 KICKOS_ARCH_SWITCH_LANDS_LATER=1)
target_compile_definitions(kickos_kseam_landing_late PRIVATE KFIXTURE_REAL_TIME=1)
kickos_add_unit_test(NAME rr_landing_late SOURCES landing.cc
  LIBRARIES kickos_kseam_landing_late
  POSTURE KICKOS_HAVE_MPU=1 KICKOS_ARCH_SWITCH_LANDS_LATER=1)

kickos_add_kseam(kickos_kseam_landing_exit EXTRA ${_time} POSTURE KICKOS_HAVE_MPU=1)
target_compile_definitions(kickos_kseam_landing_exit PRIVATE KFIXTURE_REAL_TIME=1)
kickos_add_unit_test(NAME rr_landing_exit SOURCES landing.cc
  LIBRARIES kickos_kseam_landing_exit
  POSTURE KICKOS_HAVE_MPU=1)
