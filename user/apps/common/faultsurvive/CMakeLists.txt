# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_FAULT_ISOLATION)
  return()
endif()
kickos_add_diagnostic_app(faultsurvive
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET faultsurvive)
  return()
endif()
target_compile_definitions(faultsurvive PRIVATE KICKOS_FS_MODE=0)

# Built everywhere the app is, so all three #if branches of main.cc stay compiled.
kickos_add_diagnostic_app(faultsurvive_ovf
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
target_compile_definitions(faultsurvive_ovf PRIVATE KICKOS_FS_MODE=1)

# Mode 2 moves SP itself, so it needs the ISA's spelling for it; main.cc #errors rather than
# degrading if this guard is loosened.
if(NOT (KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "x86_64"))
  kickos_add_diagnostic_app(faultsurvive_off
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(faultsurvive_off PRIVATE KICKOS_FS_MODE=2)
endif()

# The trap-stack security regression: an unprivileged thread aims its SP at a kernel word and
# traps. Only rv32imac and rxv3 have a SOFTWARE trap prologue to fool; main.cc #errors if an
# image is built anywhere else.
if(KICKOS_ARCH STREQUAL "rv32imac" OR KICKOS_ARCH STREQUAL "rxv3")
  kickos_add_diagnostic_app(faultsurvive_kwrite
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(faultsurvive_kwrite PRIVATE KICKOS_FS_MODE=3)

  # Mode 5 drops SP two bytes: in bounds, deep inside the stack, and misaligned, so only the
  # prologue's alignment leg can refuse it.
  kickos_add_diagnostic_app(faultsurvive_misalign
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(faultsurvive_misalign PRIVATE KICKOS_FS_MODE=5)
endif()

# Mode 4 parks SP at the low edge of the stack, with less room beneath it than the frame plus the
# kernel descent would need, and expects the kill anyway. It PINS rv32imac's frame and depth
# figures from rv_trap_stack.h; main.cc #errors if an image is built anywhere else.
if(KICKOS_ARCH STREQUAL "rv32imac")
  kickos_add_diagnostic_app(faultsurvive_lowedge
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(faultsurvive_lowedge PRIVATE KICKOS_FS_MODE=4)
endif()

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
list(APPEND _fs_boards ${KICKOS_QEMU_ARM64_BOARDS} qemu-riscv qemu-riscv64 qemu-x86_64 sim)
if(NOT KICKOS_BOARD IN_LIST _fs_boards)
  return()
endif()
if(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  set(_fs_tag ${KICKOS_QEMU_MPS2_TAG})
else()
  string(REPLACE "-" "_" _fs_tag ${KICKOS_BOARD})
endif()

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME ${_fs_tag}_faultsurvive
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
            "$<TARGET_FILE:faultsurvive>" survive ${KICKOS_ARCH} terminated)
  set_tests_properties(${_fs_tag}_faultsurvive PROPERTIES TIMEOUT 30)
  # The gate builds its own tree, one service-list provider linking per image; the
  # services_none guard keeps it from registering inside the tree it configures and recursing.
  if(KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
    add_test(NAME ${_fs_tag}_faultsurvive_published
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_faultsurvive_pub.sh"
              "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
    set_tests_properties(${_fs_tag}_faultsurvive_published PROPERTIES TIMEOUT 300)
  endif()
else()
  kickos_add_qemu_test(NAME ${_fs_tag}_faultsurvive TARGET faultsurvive
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS survive ${KICKOS_ARCH} terminated)
endif()

set(_fs_outcome terminated)
if(KICKOS_ARCH STREQUAL "rv32imac")
  set(_fs_outcome contained)
endif()

if(KICKOS_ARCH STREQUAL "sim" OR NOT KICKOS_HAVE_MPU)
  return()
endif()
kickos_add_qemu_test(NAME ${_fs_tag}_faultoverflow TARGET faultsurvive_ovf
  BOARD ${KICKOS_BOARD}
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS overflow ${KICKOS_ARCH} ${_fs_outcome})
kickos_add_qemu_test(NAME ${_fs_tag}_faultoffstack TARGET faultsurvive_off
  BOARD ${KICKOS_BOARD}
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS offstack ${KICKOS_ARCH} ${_fs_outcome})

if(KICKOS_ARCH STREQUAL "rv32imac")
  kickos_add_qemu_test(NAME ${_fs_tag}_faultkwrite TARGET faultsurvive_kwrite
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS kwrite ${KICKOS_ARCH} ${_fs_outcome})
  kickos_add_qemu_test(NAME ${_fs_tag}_faultmisalign TARGET faultsurvive_misalign
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS misalign ${KICKOS_ARCH} ${_fs_outcome})
endif()

if(KICKOS_ARCH STREQUAL "rv32imac")
  kickos_add_qemu_test(NAME ${_fs_tag}_faultlowedge TARGET faultsurvive_lowedge
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh" ARGS lowedge ${KICKOS_ARCH} terminated)
endif()
