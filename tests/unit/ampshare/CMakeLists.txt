# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The AMP partition's user share on a region board, over the REAL kernel/grant/grant.cc and
# kernel/mem/ramown.cc at an own-image node's posture with a share stated. No region board with
# a share has an emulator, so this is where the seat and the confinement exception run.
kickos_add_unit_test(NAME amp_share_admission
  SOURCES share_admission.cc
          "${PROJECT_SOURCE_DIR}/kernel/grant/grant.cc"
          "${PROJECT_SOURCE_DIR}/kernel/mem/ramown.cc"
  POSTURE KICKOS_AMP_OWN_IMAGE=1
          KICKOS_AMP_USER_SHARE_ADDR=0x2007C000
          KICKOS_AMP_USER_SHARE_SIZE=0x4000)

# The translating board's admission of a window over the share its holder reserves, over the REAL
# kernel/amp/ampshare.cc and kernel/mem/vrange.cc. The share's holder is the init, which no image
# arm can be.
kickos_add_unit_test(NAME amp_share_window
  SOURCES share_window.cc
          "${PROJECT_SOURCE_DIR}/kernel/amp/ampshare.cc"
          "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
  POSTURE KICKOS_HAVE_ASPACE=1
          KICKOS_AMP_OWN_IMAGE=1
          KICKOS_AMP_NODES=2
          KICKOS_AMP_NODE_ID=0
          KICKOS_AMP_PARTITION_BASE=0x40000000
          KICKOS_AMP_NODE_SHARE=0x100000
          KICKOS_AMP_SHARED_SIZE=0x20000
          KICKOS_AMP_USER_SHARE_ADDR=0x40210000
          KICKOS_AMP_USER_SHARE_SIZE=0x10000)

# The share's one memory type in each backend's real type rule. The rule's TU is linked with
# --gc-sections: only the rule's own callees need a seam.
kickos_add_unit_test(NAME amp_share_type_aspace
  SOURCES share_type.cc
          "${PROJECT_SOURCE_DIR}/kernel/mem/aspace.cc"
          "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
  POSTURE KICKOS_HAVE_ASPACE=1
          KICKOS_AMP_OWN_IMAGE=1
          KICKOS_AMP_NODES=2
          KICKOS_AMP_NODE_ID=0
          KICKOS_AMP_PARTITION_BASE=0x40000000
          KICKOS_AMP_NODE_SHARE=0x100000
          KICKOS_AMP_SHARED_SIZE=0x20000
          KICKOS_AMP_USER_SHARE_ADDR=0x40210000
          KICKOS_AMP_USER_SHARE_SIZE=0x10000
          KICKOS_AMP_USER_SHARE_UNCACHED=1)
kickos_add_unit_test(NAME amp_share_type_aspace_cached TEST_PREFIX cached_
  SOURCES share_type.cc
          "${PROJECT_SOURCE_DIR}/kernel/mem/aspace.cc"
          "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
  POSTURE KICKOS_HAVE_ASPACE=1
          KICKOS_AMP_OWN_IMAGE=1
          KICKOS_AMP_NODES=2
          KICKOS_AMP_NODE_ID=0
          KICKOS_AMP_PARTITION_BASE=0x40000000
          KICKOS_AMP_NODE_SHARE=0x100000
          KICKOS_AMP_SHARED_SIZE=0x20000
          KICKOS_AMP_USER_SHARE_ADDR=0x40210000
          KICKOS_AMP_USER_SHARE_SIZE=0x10000)
kickos_add_unit_test(NAME amp_share_type_region
  SOURCES share_type.cc
          "${PROJECT_SOURCE_DIR}/kernel/thread/thread.cc"
  POSTURE KICKOS_AMP_OWN_IMAGE=1
          KICKOS_AMP_USER_SHARE_ADDR=0x2007C000
          KICKOS_AMP_USER_SHARE_SIZE=0x4000)
foreach(_kickos_share_type amp_share_type_aspace amp_share_type_aspace_cached amp_share_type_region)
  target_compile_options(${_kickos_share_type} PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(${_kickos_share_type} PRIVATE -Wl,--gc-sections)
endforeach()
set_source_files_properties("${PROJECT_SOURCE_DIR}/kernel/thread/thread.cc" PROPERTIES
  COMPILE_OPTIONS "-ffreestanding;-fno-exceptions;-fno-rtti;-fno-threadsafe-statics")
