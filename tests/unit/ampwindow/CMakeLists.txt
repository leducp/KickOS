# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The validation an AMP node owes the far side's writing, over the real kernel/amp sources,
# and amp_seam.cc answers their whole undefined set.
#
# THE SOURCE SET IS READ OFF kickos_kernel RATHER THAN LISTED: the window calls into the
# partition's node-to-core map, so a list naming one of the two files fails to link.
#
# AT FOUR NODES UNDER THE SHARED-IMAGE POSTURE, which is why this gate compiles its own copy
# rather than joining the K-seam fixture: that library takes the preset's core count and its
# posture, where the whole translation unit under test compiles to nothing.
get_target_property(_ampw_kernel_srcs kickos_kernel SOURCES)
list(FILTER _ampw_kernel_srcs INCLUDE REGEX "(^|/)amp/[^/]+\\.cc$")
list(TRANSFORM _ampw_kernel_srcs PREPEND "${PROJECT_SOURCE_DIR}/kernel/")
if(_ampw_kernel_srcs STREQUAL "")
  message(FATAL_ERROR
    "KickOS: kickos_kernel lists no kernel/amp source, so amp_window would test nothing. "
    "The filter above reads that target's own SOURCES; a moved directory breaks it silently.")
endif()

# The generated amp_ports.h answers whatever the PRESET states and sim is a node of nothing.
# It guards with #ifndef, so the port values below win.
kickos_add_unit_test(NAME amp_window
  SOURCES window.cc amp_seam.cc ${_ampw_kernel_srcs}
  POSTURE KICKOS_NUM_CORES=4
          KICKOS_KERNEL_CORES=1
          KICKOS_MULTICORE_MODEL_SHARED=0
          KICKOS_AMP_NODE=1
          KICKOS_AMP_NODES=4
          KICKOS_AMP_PORT_COUNT=2
          KICKOS_AMP_SELF_NODE=0
          "KICKOS_AMP_PORT_NODE_LIST=0,1"
          "KICKOS_AMP_PORT_PORT_LIST=2,3")
