# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The validation an AMP node owes the far side's writing, over the real
# kernel/amp/ampwindow.cc, and amp_seam.cc answers its whole undefined set.
#
# AT FOUR NODES UNDER THE SHARED-IMAGE POSTURE, which is why this gate builds its own object
# rather than joining the K-seam fixture: that library takes the preset's core count and its
# posture, where the whole translation unit under test compiles to nothing.
#
# The core counts, THE MODEL and THE POSTURE reach C through the root's add_compile_definitions,
# which this directory INHERITS, so each is dropped from the inherited copy rather than shadowed:
# two -D of one macro with different values is a redefinition -Werror refuses.
# TWO PLACES CARRY IT: this directory inherited the root's copy, and kickos_kernel's own
# COMPILE_DEFINITIONS was seeded from the same place when that target was created.
get_directory_property(_ampw_dirdefs COMPILE_DEFINITIONS)
list(FILTER _ampw_dirdefs EXCLUDE REGEX "^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|AMP_NODES?)=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_ampw_dirdefs}")

kickos_add_unit_test(NAME amp_window
  SOURCES window.cc amp_seam.cc
          "${PROJECT_SOURCE_DIR}/kernel/amp/ampwindow.cc"
  INCLUDES "$<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>"
  DEFINITIONS "$<FILTER:$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>,EXCLUDE,^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|AMP_NODES?)=>"
              KICKOS_NUM_CORES=4
              KICKOS_KERNEL_CORES=1
              KICKOS_MULTICORE_MODEL_SHARED=0
              KICKOS_AMP_NODE=1
              KICKOS_AMP_NODES=4)
