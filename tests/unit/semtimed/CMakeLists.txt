# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The plain word pair over the real time.cc: a wait against a wake, a cancel and its own deadline,
# at one and two kernel cores, and over the compare-and-swap of an arch whose store can be lost,
# where an arm changes the word between the wake's load and its swap. Host-only of necessity: the
# wake and the expiry must reach one park in a chosen order, which no image can arrange. The arms
# SEQUENCE the two, they do not race them: that they never interleave rests on both taking the
# kernel's exclusion, which the debug assertions at the queue's pop and at the expiry's unlink
# check at two kernel cores.
#
# Needs kickos_kseam_time (tests/unit/parkdeath) to exist before this directory is added.
kickos_add_kseam(kickos_kseam_time_smp EXTRA "${PROJECT_SOURCE_DIR}/kernel/time/time.cc"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1)
target_compile_definitions(kickos_kseam_time_smp PRIVATE KFIXTURE_REAL_TIME=1)
kickos_add_unit_test(NAME word_timed SOURCES word_timed.cc
  LIBRARIES kickos_kseam_time)
kickos_add_unit_test(NAME word_timed_smp SOURCES word_timed.cc
  LIBRARIES kickos_kseam_time_smp
  TEST_PREFIX smp.)
kickos_add_kseam(kickos_kseam_time_bounded EXTRA "${PROJECT_SOURCE_DIR}/kernel/time/time.cc")
target_compile_definitions(kickos_kseam_time_bounded PRIVATE KFIXTURE_REAL_TIME=1)
kickos_add_unit_test(NAME word_timed_bounded SOURCES word_timed.cc TEST_PREFIX bounded.
  DEFINITIONS WORDTIMED_BOUNDED=1
  LIBRARIES kickos_kseam_time_bounded)
foreach(_bounded IN ITEMS kickos_kseam_time_bounded word_timed_bounded)
  target_include_directories(${_bounded} BEFORE PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/unit/wordref/include")
endforeach()
