# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The validation an AMP node owes the far side's writing, over the real kernel/amp sources,
# and amp_seam.cc answers their whole undefined set.
#
# THE SOURCE SET IS READ OFF kickos_kernel RATHER THAN LISTED: the window calls into the
# partition's node-to-core map, so a list naming one of the two files fails to link.
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
list(FILTER _ampw_dirdefs EXCLUDE REGEX "^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|AMP_NODES?|AMP_PORT_[A-Z_]+|AMP_SELF_NODE)=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_ampw_dirdefs}")

get_target_property(_ampw_kernel_srcs kickos_kernel SOURCES)
list(FILTER _ampw_kernel_srcs INCLUDE REGEX "(^|/)amp/[^/]+\\.cc$")
list(TRANSFORM _ampw_kernel_srcs PREPEND "${PROJECT_SOURCE_DIR}/kernel/")
if(_ampw_kernel_srcs STREQUAL "")
  message(FATAL_ERROR
    "KickOS: kickos_kernel lists no kernel/amp source, so amp_window would test nothing. "
    "The filter above reads that target's own SOURCES; a moved directory breaks it silently.")
endif()

kickos_add_unit_test(NAME amp_window
  SOURCES window.cc amp_seam.cc ${_ampw_kernel_srcs}
  INCLUDES "$<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>"
  DEFINITIONS "$<FILTER:$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>,EXCLUDE,^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|AMP_NODES?|AMP_PORT_[A-Z_]+|AMP_SELF_NODE)=>"
              KICKOS_NUM_CORES=4
              KICKOS_KERNEL_CORES=1
              KICKOS_MULTICORE_MODEL_SHARED=0
              KICKOS_AMP_NODE=1
              KICKOS_AMP_NODES=4
              # A partition of its own: the generated amp_ports.h answers whatever the PRESET
              # states and sim is a node of nothing. It guards with #ifndef, so these win.
              KICKOS_AMP_PORT_COUNT=2
              KICKOS_AMP_SELF_NODE=0
              "KICKOS_AMP_PORT_NODE_LIST=0,1"
              "KICKOS_AMP_PORT_PORT_LIST=2,3")
