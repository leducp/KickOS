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

# One gate, one executable. NAME is both the target and the ctest name.
function(kickos_add_kseam_gate)
  cmake_parse_arguments(_G "" "NAME;PANIC" "SOURCES;DEFINITIONS" ${ARGN})
  add_executable(${_G_NAME} ${_G_SOURCES})
  target_link_libraries(${_G_NAME} PRIVATE kickos_kseam)
  target_compile_options(${_G_NAME} PRIVATE ${KICKOS_WARN_FLAGS})
  if(_G_DEFINITIONS)
    target_compile_definitions(${_G_NAME} PRIVATE ${_G_DEFINITIONS})
  endif()
  add_test(NAME ${_G_NAME} COMMAND "$<TARGET_FILE:${_G_NAME}>")
  # On EVERY gate, not just the PANIC ones. A PANIC case needs it because
  # PASS_REGULAR_EXPRESSION makes ctest ignore the return code entirely; an ordinary gate
  # needs it because a second failure signal is what keeps one forged exit code from passing.
  # The cost: a gate whose PASSING output contains "not ok" cannot be registered here, which
  # rules out quoting a TAP-shaped expectation in a diff printer.
  set_tests_properties(${_G_NAME} PROPERTIES TIMEOUT 30 LABELS host
                       FAIL_REGULAR_EXPRESSION "not ok")
  if(_G_PANIC)
    set_tests_properties(${_G_NAME} PROPERTIES
      PASS_REGULAR_EXPRESSION "KERNEL PANIC: ${_G_PANIC}")
  endif()
endfunction()
