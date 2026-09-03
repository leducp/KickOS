# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The exited-slot reclaim window. The K-seam sources again, AT TWO KERNEL CORES and with
# kernel/sync/klock.cc beside them, which is why this gate builds its own object library
# rather than joining kickos_kseam: that library takes the preset's core count, and the sim
# preset drives one core, where every body of the lock compiles to nothing and the span this
# gate asserts is not expressible.
#
# The core counts AND THE MODEL reach C through the root's add_compile_definitions, which
# this directory INHERITS, so each is dropped from the inherited copy rather than shadowed: two
# -D of one macro with different values is a redefinition -Werror refuses.
# TWO PLACES CARRY IT: this directory inherited the root's copy, and kickos_kernel's own
# COMPILE_DEFINITIONS was seeded from the same place when that target was created.
get_directory_property(_exitq_dirdefs COMPILE_DEFINITIONS)
list(FILTER _exitq_dirdefs EXCLUDE REGEX "^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED)=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_exitq_dirdefs}")

set(_exitq_fixture "${PROJECT_SOURCE_DIR}/tests/unit/kfixture")

# Same source list as kickos_kseam, and the same reason for every flag on it: these are real
# kernel translation units, so they take the kernel target's include path and definitions
# verbatim, and the kernel's COMPILE_OPTIONS are left off because this is a hosted program.
add_library(kickos_kseam_smp OBJECT
  "${_exitq_fixture}/kfixture.cc"
  "${_exitq_fixture}/karch_seam.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sched/sched.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sched/policy_fifo_rr.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sync/sync.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sync/klock.cc"
  "${PROJECT_SOURCE_DIR}/kernel/thread/park.cc"
  "${PROJECT_SOURCE_DIR}/kernel/syscall/cap.cc"
  "${PROJECT_SOURCE_DIR}/kernel/task/task.cc"
  "${PROJECT_SOURCE_DIR}/kernel/irq/irq.cc"
  "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
  "${PROJECT_SOURCE_DIR}/kernel/init/fault.cc")

target_compile_features(kickos_kseam_smp PUBLIC ${KICKOS_CXX_STANDARD})
target_include_directories(kickos_kseam_smp PUBLIC
  $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES> "${_exitq_fixture}")
target_compile_definitions(kickos_kseam_smp PUBLIC
  "$<FILTER:$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>,EXCLUDE,^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED)=>"
  KICKOS_NUM_CORES=2
  KICKOS_KERNEL_CORES=2
  KICKOS_MULTICORE_MODEL_SHARED=1)
target_compile_options(kickos_kseam_smp PRIVATE ${KICKOS_WARN_FLAGS}
  -fno-exceptions -fno-rtti -fno-threadsafe-statics)

add_executable(exit_quiesce exit_span.cc)
target_link_libraries(exit_quiesce PRIVATE kickos_kseam_smp GTest::gtest_main)
target_compile_options(exit_quiesce PRIVATE ${KICKOS_WARN_FLAGS})
kickos_discover_unit_tests(exit_quiesce)
