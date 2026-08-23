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

# Mode 2 moves SP itself, which needs the ISA's spelling for it (see the sim note below).
# main.cc #errors rather than degrading if this guard is loosened.
if(NOT KICKOS_ARCH STREQUAL "sim")
  kickos_add_diagnostic_app(faultsurvive_off
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(faultsurvive_off PRIVATE KICKOS_FS_MODE=2)
endif()

# The trap-stack security regression: an unprivileged thread aims its SP at a kernel word
# and traps. Only the two backends with a SOFTWARE trap prologue (rv32imac, rxv3) can be
# fooled into writing through a U-mode SP; the hardware-stacking ARM cores and the sim have
# no such prologue, and main.cc #errors if an image is built there.
if(KICKOS_ARCH STREQUAL "rv32imac" OR KICKOS_ARCH STREQUAL "rxv3")
  kickos_add_diagnostic_app(faultsurvive_kwrite
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(faultsurvive_kwrite PRIVATE KICKOS_FS_MODE=3)

  # Mode 5 drops SP two bytes: in bounds, deep inside the stack, and misaligned, so only the
  # prologue's alignment leg can refuse it. Same two backends as mode 3, same per-ISA SP
  # spelling.
  kickos_add_diagnostic_app(faultsurvive_misalign
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(faultsurvive_misalign PRIVATE KICKOS_FS_MODE=5)
endif()

# Mode 4 parks SP inside the stack with too little room beneath it for the frame PLUS the
# kernel descent. It PINS rv32imac's frame and red-zone figures from rv_trap_stack.h, so it
# is that backend's alone: rxv3 stacks its fault frame on the ISP and defines no such
# geometry, and main.cc #errors if an image is built there.
if(KICKOS_ARCH STREQUAL "rv32imac")
  kickos_add_diagnostic_app(faultsurvive_lowedge
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(faultsurvive_lowedge PRIVATE KICKOS_FS_MODE=4)
endif()

# arch-private include path, on these two targets only.
if(KICKOS_ARCH STREQUAL "rv32imac")
  foreach(_fs_tgt faultsurvive_kwrite faultsurvive_lowedge)
    target_include_directories(${_fs_tgt} PRIVATE
      "${PROJECT_SOURCE_DIR}/arch/riscv/rv32imac/include")
  endforeach()
endif()

if(NOT KICKOS_BUILD_TESTS)
  return()
endif()

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
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
            "$<TARGET_FILE:faultsurvive>" survive ${KICKOS_ARCH})
  set_tests_properties(${_fs_tag}_faultsurvive PROPERTIES TIMEOUT 30)
  # Same arm across a console HANDOVER, which the arm above never crosses: every other
  # faultsurvive runner keeps the console kernel-owned, and a published one DROPS the kernel
  # chip path, so this is the posture where the record used to be lost. The gate builds its
  # own tree (one service-list provider links per image), hence the services_none guard:
  # registering it inside the tree it configures would recurse.
  if(KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
    add_test(NAME ${_fs_tag}_faultsurvive_published
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_faultsurvive_pub.sh"
              "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
    set_tests_properties(${_fs_tag}_faultsurvive_published PROPERTIES TIMEOUT 300)
  endif()
else()
  kickos_add_qemu_test(NAME ${_fs_tag}_faultsurvive TARGET faultsurvive
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS survive ${KICKOS_ARCH})
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
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS overflow ${KICKOS_ARCH})
kickos_add_qemu_test(NAME ${_fs_tag}_faultoffstack TARGET faultsurvive_off
  BOARD ${KICKOS_BOARD}
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS offstack ${KICKOS_ARCH})

# The kernel-write regression. Only qemu-riscv reaches here; rx72m builds the image above
# and is witnessed on silicon through FS_CAPTURE.
if(KICKOS_ARCH STREQUAL "rv32imac")
  kickos_add_qemu_test(NAME ${_fs_tag}_faultkwrite TARGET faultsurvive_kwrite
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS kwrite ${KICKOS_ARCH})
  kickos_add_qemu_test(NAME ${_fs_tag}_faultmisalign TARGET faultsurvive_misalign
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS misalign ${KICKOS_ARCH})
endif()

# The kernel-descent arm, wherever its image was built.
if(KICKOS_ARCH STREQUAL "rv32imac")
  kickos_add_qemu_test(NAME ${_fs_tag}_faultlowedge TARGET faultsurvive_lowedge
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS lowedge ${KICKOS_ARCH})
endif()
