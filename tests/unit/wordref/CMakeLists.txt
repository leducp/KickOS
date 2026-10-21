# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A user word the kernel names on a board that does not translate, over the REAL
# kernel/syscall/word.cc and syscall_mem.cc on the K-seam, at the three postures it ships at:
# the sim's, where the host's own memory lies outside the arena; a region board's; and a flat
# board's, where nothing is enforced. The translating posture's gate is sys_word_ref. A region
# board's again with an AMP partition's user share stated. The sim's posture again over the
# bounded compare-and-swap an arch whose store can be lost builds, with a store attempt the test
# makes lose.
set(_word "${PROJECT_SOURCE_DIR}/kernel/syscall/word.cc"
          "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_mem.cc")
kickos_add_kseam(kickos_kseam_word_sim
  EXTRA ${_word}
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall")
kickos_add_kseam(kickos_kseam_word_region
  EXTRA ${_word}
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  POSTURE KICKOS_ARCH_SIM=0)
kickos_add_kseam(kickos_kseam_word_flat
  EXTRA ${_word}
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  POSTURE KICKOS_ARCH_SIM=0 KICKOS_MEMORY_ENFORCED=0 KICKOS_HAVE_MPU=0)
kickos_add_kseam(kickos_kseam_word_share
  EXTRA ${_word}
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  POSTURE KICKOS_ARCH_SIM=0 KICKOS_AMP_OWN_IMAGE=1
          KICKOS_AMP_USER_SHARE_ADDR=0x5a0000000 KICKOS_AMP_USER_SHARE_SIZE=0x4000)
kickos_add_kseam(kickos_kseam_word_bounded
  EXTRA ${_word}
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall")
kickos_add_unit_test(NAME word_sim SOURCES word_classes.cc TEST_PREFIX sim_
  LIBRARIES kickos_kseam_word_sim)
kickos_add_unit_test(NAME word_region SOURCES word_classes.cc TEST_PREFIX region_
  LIBRARIES kickos_kseam_word_region)
kickos_add_unit_test(NAME word_flat SOURCES word_classes.cc TEST_PREFIX flat_
  LIBRARIES kickos_kseam_word_flat)
kickos_add_unit_test(NAME word_share SOURCES word_classes.cc TEST_PREFIX share_
  LIBRARIES kickos_kseam_word_share)
kickos_add_unit_test(NAME word_bounded SOURCES word_classes.cc TEST_PREFIX bounded_
  DEFINITIONS WORDREF_BOUNDED=1
  LIBRARIES kickos_kseam_word_bounded)
foreach(_bounded IN ITEMS kickos_kseam_word_bounded word_bounded)
  target_include_directories(${_bounded} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
endforeach()
