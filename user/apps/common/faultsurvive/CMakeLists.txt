# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Gated on the runtime flag itself, so a backend cannot opt into fault isolation and leave
# its witness behind. main.cc carries the per-ISA undefined-instruction spelling.
if(NOT KICKOS_FAULT_ISOLATION)
  return()
endif()
kickos_add_diagnostic_app(faultsurvive
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET faultsurvive)
  return() # diagnostics off -> app not built; skip its tests
endif()
target_compile_definitions(faultsurvive PRIVATE KICKOS_FS_MODE=0)

# The overflow and off-stack arms are separate binaries because they end the process, and
# they are BUILT everywhere the app is so all three #if branches of main.cc stay compiled.
kickos_add_diagnostic_app(faultsurvive_ovf
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
target_compile_definitions(faultsurvive_ovf PRIVATE KICKOS_FS_MODE=1)

# Mode 2 moves SP itself, which needs the ISA's spelling for it and has no host form:
# the sim's seam rewrites a ucontext rather than a guest frame, so there is no frame it
# could invalidate. main.cc #errors rather than degrading if this guard is ever loosened.
if(NOT KICKOS_ARCH STREQUAL "sim")
  kickos_add_diagnostic_app(faultsurvive_off
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(faultsurvive_off PRIVATE KICKOS_FS_MODE=2)
endif()

if(NOT KICKOS_BUILD_TESTS)
  return()
endif()

# Which runner this board takes.
set(_fs_boards ${KICKOS_QEMU_MPS2_BOARDS})
list(APPEND _fs_boards qemu-riscv sim)
if(NOT KICKOS_BOARD IN_LIST _fs_boards)
  return()
endif()
if(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  set(_fs_tag ${KICKOS_QEMU_MPS2_TAG})
else()
  string(REPLACE "-" "_" _fs_tag ${KICKOS_BOARD})
endif()

# The survive arm is registered in BOTH postures. Privilege comes from arch_context_init
# and the worker is spawned unprivileged either way, so a flat board kills it exactly as
# an enforcing one does; nothing here depends on the MPU.
if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME ${_fs_tag}_faultsurvive
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_faultsurvive.sh"
            "$<TARGET_FILE:faultsurvive>" survive ${KICKOS_ARCH})
  set_tests_properties(${_fs_tag}_faultsurvive PROPERTIES TIMEOUT 30)
else()
  kickos_add_qemu_test(NAME ${_fs_tag}_faultsurvive TARGET faultsurvive
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_faultsurvive.sh" ARGS survive ${KICKOS_ARCH})
endif()

# The two REFUSAL arms need a hardware stack guard: they turn on the frame the fault
# handler is handed being untrustworthy, and only a trapping guard produces one.
#
# The sim has neither. Its thread stacks are plain host allocations with no guard page
# (arch_context_init substitutes a 64 KiB malloc block below KICKOS_MIN_STACK_SIZE, so
# Thread::stack_base does not even describe the stack in use), and its seam rewrites a
# host ucontext rather than a guest frame, so there is no frame-validity test on that
# backend to witness. Registering these there would assert nothing.
if(KICKOS_ARCH STREQUAL "sim" OR NOT KICKOS_HAVE_MPU)
  return()
endif()
kickos_add_qemu_test(NAME ${_fs_tag}_faultoverflow TARGET faultsurvive_ovf
  BOARD ${KICKOS_BOARD}
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_faultsurvive.sh" ARGS overflow ${KICKOS_ARCH})
kickos_add_qemu_test(NAME ${_fs_tag}_faultoffstack TARGET faultsurvive_off
  BOARD ${KICKOS_BOARD}
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_faultsurvive.sh" ARGS offstack ${KICKOS_ARCH})
