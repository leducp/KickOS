# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kernel/init/terminal.cc at an own-image AMP node's posture, once as the partition's reset owner
# and once as another node: only the owner's kernel admits a reset.
set(_ampreset_posture KICKOS_NUM_CORES=1 KICKOS_KERNEL_CORES=1 KICKOS_MULTICORE_MODEL_SHARED=0
    KICKOS_AMP_NODE=1 KICKOS_AMP_OWN_IMAGE=1 KICKOS_AMP_NODES=2 KICKOS_AMP_RESET_OWNER=0
    KICKOS_AMP_PORT_COUNT=1 KICKOS_AMP_SELF_NODE=0 "KICKOS_AMP_PORT_NODE_LIST=0"
    "KICKOS_AMP_PORT_PORT_LIST=2")
set(_ampreset_sources reset_owner.cc reset_seam.cc "${PROJECT_SOURCE_DIR}/kernel/init/terminal.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sync/klock.cc")
kickos_add_unit_test(NAME amp_reset_owner
  SOURCES ${_ampreset_sources}
  POSTURE ${_ampreset_posture} KICKOS_AMP_NODE_ID=0)
kickos_add_unit_test(NAME amp_reset_other
  SOURCES ${_ampreset_sources}
  POSTURE ${_ampreset_posture} KICKOS_AMP_NODE_ID=1)
