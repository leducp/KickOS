# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The core-mask admission, AT FOUR KERNEL CORES WITH TWO OF THEM ISOLATED. No preset carries
# that shape: qemu-arm64-smpiso isolates ONE core, and a mask naming SEVERAL isolated cores, the
# pool an explicit affinity reserves, needs two.
#
# Header-only: sched.h carries the whole rule, so this gate compiles no kernel source and the
# core count reaches it through definitions alone.
kickos_add_unit_test(NAME admit_mask SOURCES admit_mask.cc
  POSTURE KICKOS_NUM_CORES=4 KICKOS_KERNEL_CORES=4 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_ISOLATED_CORES=0xC)
