# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Neither the aspace nor the libc reentrant posture comes from the sim preset.
set(_reent "${PROJECT_SOURCE_DIR}/kernel/mem/aspace.cc"
           "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
           "${PROJECT_SOURCE_DIR}/kernel/thread/reent.cc")
set_source_files_properties(${_reent} DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
kickos_add_kseam(kickos_kseam_reent
  EXTRA "${PROJECT_SOURCE_DIR}/tests/unit/deathspace/aspace_seam.cc" ${_reent}
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/deathspace"
  POSTURE KICKOS_HAVE_ASPACE=1 KICKOS_LIBC_REENT=1 KICKOS_REENT_PER_THREAD=1
          KICKOS_REENT_IN_TCB=0)
kickos_add_unit_test(NAME reent_seat SOURCES reent_seat.cc
  LIBRARIES kickos_kseam_reent)
target_link_options(reent_seat PRIVATE -Wl,--gc-sections)
