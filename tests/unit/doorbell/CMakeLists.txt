# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The shared doorbell park, poll and turn wait, instantiated from
# arch/include/kickos/arch/doorbell_protocol.h over a host part, at two kernel cores.
kickos_add_unit_test(NAME doorbell_templates
  SOURCES seam.cc park.cc poll.cc
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/include"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_DOORBELL_CORES=2
          KICKOS_MULTICORE_MODEL_SHARED=1)
