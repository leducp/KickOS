# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A dying member vacates its address space before it drops the reference that can destroy it.
# The K-seam sources AGAIN, at two kernel cores AND with KICKOS_HAVE_ASPACE=1: no board the
# host arch answers for translates (arch/Kconfig, HAS_ASPACE), so the definition cannot come
# from the preset, and the exit path's whole aspace arm is compiled out of both shared K-seam
# libraries.
#
# aspace_seam.cc stands in for the kernel/mem layer, which is not compiled here: what this
# gate carries is the ORDER of the calls exit_current makes across that boundary, and the
# bodies below it are the target's to witness.
kickos_add_kseam(kickos_kseam_aspace
  EXTRA aspace_seam.cc
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_HAVE_ASPACE=1)
kickos_add_unit_test(NAME leave_space SOURCES leave_space.cc
  LIBRARIES kickos_kseam_aspace)

# THE SAME SOURCES AT ONE KERNEL CORE. exit_current's reroot arm is guarded on
# KICKOS_KERNEL_CORES > 1, so the library above compiles the arm IN and can witness only its
# presence; the guard itself needs a translation unit where the macro is 1.
kickos_add_kseam(kickos_kseam_aspace_one
  EXTRA aspace_seam.cc
  POSTURE KICKOS_NUM_CORES=1 KICKOS_KERNEL_CORES=1 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_HAVE_ASPACE=1)
kickos_add_unit_test(NAME leave_space_one SOURCES leave_space_one.cc
  LIBRARIES kickos_kseam_aspace_one)
