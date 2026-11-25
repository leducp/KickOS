# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The user's console contract riding `ampcon`: on one kernel, and through another node's relay.

if(NOT TARGET ampcon)
  return()
endif()
if(NOT KICKOS_AMP_NODE)
  kickos_add_qemu_test(TARGET ampcon SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_ampcon.sh"
                       ARGS local)
  return()
endif()

kickos_unbooted(ampcon "the partition boots the copy a peer tree of its own builds")
if(NOT KICKOS_AMP_NODE_ID EQUAL 0)
  kickos_unbooted(ampcon_n0 "node 0's build boots the partition, not this node's")
  return()
endif()
get_target_property(_ampcon_artefact ampcon_n0 KICKOS_AMP_ARTEFACT)
# It begins by building the artefact, which may not race another build of this tree.
kickos_add_qemu_test(NAME amp_console TARGET ampcon_n0
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_ampcon.sh"
  ARGS partition "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${_ampcon_artefact}"
  WORK 900)
if(TEST amp_console)
  set_tests_properties(amp_console PROPERTIES RUN_SERIAL TRUE)
endif()
