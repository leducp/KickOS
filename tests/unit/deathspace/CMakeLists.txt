# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A dying member vacates its address space before it drops the reference that can destroy it.
# The K-seam sources AGAIN, at two kernel cores AND with KICKOS_HAVE_ASPACE=1, which is why
# this gate builds its own object library rather than joining kickos_kseam or
# kickos_kseam_smp: no board the host arch answers for translates (arch/Kconfig, HAS_ASPACE),
# so the definition cannot come from the preset, and the exit path's whole aspace arm is
# compiled out of both existing libraries.
#
# aspace_seam.cc stands in for the kernel/mem layer, which is not compiled here: what this
# gate carries is the ORDER of the calls exit_current makes across that boundary, and the
# bodies below it are the target's to witness.
#
# The core counts AND THE MODEL reach C through the root's add_compile_definitions, which this
# directory INHERITS, so each is dropped from the inherited copy rather than shadowed: two -D
# of one macro with different values is a redefinition -Werror refuses. TWO PLACES CARRY IT:
# this directory inherited the root's copy, and kickos_kernel's own COMPILE_DEFINITIONS was
# seeded from the same place when that target was created.
get_directory_property(_deathsp_dirdefs COMPILE_DEFINITIONS)
list(FILTER _deathsp_dirdefs EXCLUDE
     REGEX "^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|HAVE_ASPACE)=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_deathsp_dirdefs}")

set(_deathsp_fixture "${PROJECT_SOURCE_DIR}/tests/unit/kfixture")

add_library(kickos_kseam_aspace OBJECT
  "${_deathsp_fixture}/kfixture.cc"
  "${_deathsp_fixture}/karch_seam.cc"
  aspace_seam.cc
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

target_compile_features(kickos_kseam_aspace PUBLIC ${KICKOS_CXX_STANDARD})
target_include_directories(kickos_kseam_aspace PUBLIC
  $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  "${_deathsp_fixture}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(kickos_kseam_aspace PUBLIC
  "$<FILTER:$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>,EXCLUDE,^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|HAVE_ASPACE)=>"
  KICKOS_NUM_CORES=2
  KICKOS_KERNEL_CORES=2
  KICKOS_MULTICORE_MODEL_SHARED=1
  KICKOS_HAVE_ASPACE=1)
target_compile_options(kickos_kseam_aspace PRIVATE ${KICKOS_WARN_FLAGS}
  -fno-exceptions -fno-rtti -fno-threadsafe-statics)

add_executable(leave_space leave_space.cc)
target_link_libraries(leave_space PRIVATE kickos_kseam_aspace GTest::gtest_main)
target_compile_options(leave_space PRIVATE ${KICKOS_WARN_FLAGS})
kickos_discover_unit_tests(leave_space)

# THE SAME SOURCES AT ONE KERNEL CORE. exit_current's reroot arm is guarded on
# KICKOS_KERNEL_CORES > 1, so the library above compiles the arm IN and can witness only its
# presence; the guard itself needs a translation unit where the macro is 1.
add_library(kickos_kseam_aspace_one OBJECT
  "${_deathsp_fixture}/kfixture.cc"
  "${_deathsp_fixture}/karch_seam.cc"
  aspace_seam.cc
  "${PROJECT_SOURCE_DIR}/kernel/sched/sched.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sched/policy_fifo_rr.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sync/sync.cc"
  "${PROJECT_SOURCE_DIR}/kernel/thread/park.cc"
  "${PROJECT_SOURCE_DIR}/kernel/syscall/cap.cc"
  "${PROJECT_SOURCE_DIR}/kernel/task/task.cc"
  "${PROJECT_SOURCE_DIR}/kernel/irq/irq.cc"
  "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
  "${PROJECT_SOURCE_DIR}/kernel/init/fault.cc")

target_compile_features(kickos_kseam_aspace_one PUBLIC ${KICKOS_CXX_STANDARD})
target_include_directories(kickos_kseam_aspace_one PUBLIC
  $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  "${_deathsp_fixture}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(kickos_kseam_aspace_one PUBLIC
  "$<FILTER:$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>,EXCLUDE,^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|HAVE_ASPACE)=>"
  KICKOS_NUM_CORES=1
  KICKOS_KERNEL_CORES=1
  KICKOS_MULTICORE_MODEL_SHARED=1
  KICKOS_HAVE_ASPACE=1)
target_compile_options(kickos_kseam_aspace_one PRIVATE ${KICKOS_WARN_FLAGS}
  -fno-exceptions -fno-rtti -fno-threadsafe-statics)

add_executable(leave_space_one leave_space_one.cc)
target_link_libraries(leave_space_one PRIVATE kickos_kseam_aspace_one GTest::gtest_main)
target_compile_options(leave_space_one PRIVATE ${KICKOS_WARN_FLAGS})
kickos_discover_unit_tests(leave_space_one)
