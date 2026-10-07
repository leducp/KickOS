# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A cancel honoured at a park prologue ends the park's bracket before the capability sweep.
# Host-only of necessity: the prologue is reached only by a cancel landing between syscall
# entry and the bracket, which no image can arrange, and the witness is the seam's bracket
# count at each chunk boundary.
#
# Needs kickos_kseam_smp (tests/unit/exitquiesce) and kickos_kseam_ipc_smp
# (tests/unit/fusedwake) to exist before this directory is added.
kickos_add_unit_test(NAME cancelled_park SOURCES sync_park.cc
  LIBRARIES kickos_kseam)
kickos_add_unit_test(NAME cancelled_park_smp SOURCES sync_park.cc
  LIBRARIES kickos_kseam_smp
  TEST_PREFIX smp.)

# The K-seam with the real time.cc in place of the seam's ktime stubs.
kickos_add_kseam(kickos_kseam_time EXTRA "${PROJECT_SOURCE_DIR}/kernel/time/time.cc")
target_compile_definitions(kickos_kseam_time PRIVATE KFIXTURE_REAL_TIME=1)
kickos_add_unit_test(NAME cancelled_timed_park SOURCES timed_park.cc
  LIBRARIES kickos_kseam_time)

kickos_add_unit_test(NAME cancelled_ipc_park SOURCES ipc_park.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  LIBRARIES kickos_kseam_ipc_smp)
