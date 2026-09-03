# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The reschedule a cross-core raise carries, against a consumer that absorbs the raise.
# kernel/sync/klock.cc is compiled here as the real kernel translation unit, and
# resched_seam.cc answers its whole undefined set.
#
# AT TWO KERNEL CORES, which is the point and is why this gate builds its own object rather
# than joining the K-seam fixture: that library takes the preset's core count, and the sim
# preset drives one core, where the mechanism under test compiles to nothing.
#
# The core counts AND THE MODEL reach C through the root's add_compile_definitions, which
# this directory INHERITS, so each is dropped from the inherited copy rather than shadowed: two
# -D of one macro with different values is a redefinition -Werror refuses.
# TWO PLACES CARRY IT: this directory inherited the root's copy, and kickos_kernel's own
# COMPILE_DEFINITIONS was seeded from the same place when that target was created.
get_directory_property(_reow_dirdefs COMPILE_DEFINITIONS)
list(FILTER _reow_dirdefs EXCLUDE REGEX "^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED)=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_reow_dirdefs}")

kickos_add_unit_test(NAME resched_owed
  SOURCES owed.cc resched_seam.cc
          "${PROJECT_SOURCE_DIR}/kernel/sync/klock.cc"
  INCLUDES "$<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>"
  DEFINITIONS "$<FILTER:$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>,EXCLUDE,^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED)=>"
              KICKOS_NUM_CORES=2
              KICKOS_KERNEL_CORES=2
              KICKOS_MULTICORE_MODEL_SHARED=1)
