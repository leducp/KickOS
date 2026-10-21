# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Neither the aspace nor the libc reentrant posture comes from the sim preset.
set(_reent "${PROJECT_SOURCE_DIR}/kernel/mem/aspace.cc"
           "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
           "${PROJECT_SOURCE_DIR}/kernel/thread/reent.cc")
set_source_files_properties(${_reent} DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
kickos_add_kseam(kickos_kseam_reent
  EXTRA "${PROJECT_SOURCE_DIR}/tests/unit/deathspace/aspace_seam.cc"
        "${PROJECT_SOURCE_DIR}/tests/unit/common/aspace_leaves.cc" ${_reent}
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/deathspace"
  POSTURE KICKOS_HAVE_ASPACE=1 KICKOS_LIBC_REENT=1 KICKOS_REENT_PER_THREAD=1
          KICKOS_REENT_IN_TCB=0)
kickos_add_unit_test(NAME reent_seat SOURCES reent_seat.cc
  LIBRARIES kickos_kseam_reent)
target_link_options(reent_seat PRIVATE -Wl,--gc-sections)
# reent_seam_read asks for the descriptor's alias: an empty image carves none.
include("${PROJECT_SOURCE_DIR}/tests/unit/common/host_image.cmake")
kickos_host_image(reent_seat TEXT g_reent_image 0 DATA g_reent_image 0)

kickos_add_kseam(kickos_kseam_reent_word
  EXTRA "${PROJECT_SOURCE_DIR}/kernel/thread/reent.cc"
  POSTURE KICKOS_HAVE_ASPACE=0 KICKOS_LIBC_REENT=1 KICKOS_REENT_PER_THREAD=0
          KICKOS_REENT_IN_TCB=0)
kickos_add_unit_test(NAME reent_word SOURCES reent_word.cc
  LIBRARIES kickos_kseam_reent_word)

# The control-block posture, at armv8a's control-block size.
kickos_add_kseam(kickos_kseam_reent_tcb
  EXTRA "${PROJECT_SOURCE_DIR}/kernel/thread/reent.cc"
  POSTURE KICKOS_HAVE_ASPACE=0 KICKOS_LIBC_REENT=1 KICKOS_REENT_PER_THREAD=1
          KICKOS_REENT_IN_TCB=1 KICKOS_ARCH_TLS_TCB=16)
kickos_add_unit_test(NAME reent_tcb SOURCES reent_tcb.cc
  LIBRARIES kickos_kseam_reent_tcb)
