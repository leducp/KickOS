# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The K-seam fixture: the kernel's stateful core compiled for the HOST, with karch_seam.cc
# answering the arch boundary. These are real kernel translation units, so they take the
# kernel target's include path and definitions VERBATIM, or the gate would compile a
# different translation unit than the one that ships.
#
# What is NOT inherited is the kernel's COMPILE_OPTIONS, and the difference is deliberate
# rather than an oversight: -ffreestanding, -fno-common and -fno-use-cxa-atexit describe an
# image with no host libc, and this gate is a hosted program that needs stdio, setjmp and
# atexit. The three LANGUAGE flags below are inherited by hand, because those change what the
# translation unit MEANS. Nothing in the tree tests __STDC_HOSTED__ or __EXCEPTIONS today, so
# the preprocessed kernel TUs are identical either way; an #if on one of those in a kernel
# header would silently diverge the gate from the shipping build.
add_library(kickos_kseam OBJECT
  kfixture.cc
  karch_seam.cc
  "${PROJECT_SOURCE_DIR}/kernel/sched/sched.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sched/policy_fifo_rr.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sync/sync.cc"
  "${PROJECT_SOURCE_DIR}/kernel/thread/park.cc"
  "${PROJECT_SOURCE_DIR}/kernel/syscall/cap.cc"
  "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc")

# PUBLIC so a gate inherits them: it includes kernel headers itself. cxx_std is NOT
# inherited with the include path, and instance.cc's constinit does not parse without it.
target_compile_features(kickos_kseam PUBLIC ${KICKOS_CXX_STANDARD})
target_include_directories(kickos_kseam PUBLIC
  $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES> "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(kickos_kseam PUBLIC
  $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
target_compile_options(kickos_kseam PRIVATE ${KICKOS_WARN_FLAGS}
  -fno-exceptions -fno-rtti -fno-threadsafe-statics)

# A panic invariant needs no separate target and no PASS_REGULAR_EXPRESSION: a gtest death test
# forks, so a case asserting a kpanic sits in the same binary as the ordinary ones.
function(kickos_add_kseam_gate)
  cmake_parse_arguments(_G "" "NAME" "SOURCES;DEFINITIONS" ${ARGN})
  add_executable(${_G_NAME} ${_G_SOURCES})
  target_link_libraries(${_G_NAME} PRIVATE kickos_kseam GTest::gtest_main)
  target_compile_options(${_G_NAME} PRIVATE ${KICKOS_WARN_FLAGS})
  if(_G_DEFINITIONS)
    target_compile_definitions(${_G_NAME} PRIVATE ${_G_DEFINITIONS})
  endif()
  kickos_discover_unit_tests(${_G_NAME})
endfunction()
