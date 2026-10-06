# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding the `faultsurvive` family: one image per fault mode, each asking that the
# violation be contained and the rest of the system carry on. Each mode has a target of its own,
# so a clause here is keyed on the same posture that built its image.

if(NOT TARGET faultsurvive)
  return()
endif()

set(_fs_outcome terminated)
if(KICKOS_ARCH STREQUAL "rv32imac")
  set(_fs_outcome contained)
endif()
# The arches the gate holds corroborating evidence for. The overflow needs a guard to fault it; the
# off-stack, kwrite and misalign arms need an unprivileged worker, which the RX runs only where
# memory is enforced.
set(_fs_judged OFF)
set(_fs_guarded OFF)
set(_fs_unprivileged OFF)
if(KICKOS_ARCH MATCHES "^(armv7m|armv6m|rv32imac|rxv3)$")
  set(_fs_judged ON)
  if(KICKOS_MEMORY_ENFORCED)
    set(_fs_guarded ON)
  endif()
  if(KICKOS_FAULT_ISOLATION AND (KICKOS_MEMORY_ENFORCED OR NOT KICKOS_ARCH STREQUAL "rxv3"))
    set(_fs_unprivileged ON)
  endif()
endif()
set(_fs_privileged_why "faults are not isolated")
if(KICKOS_FAULT_ISOLATION)
  set(_fs_privileged_why "tasks run privileged, so the privilege test refuses the frame first")
endif()

if(_fs_judged)
  kickos_app_judge(faultsurvive tests/integration/check_faultsurvive.sh
    ARGS survive ${KICKOS_ARCH} terminated)
  if(_fs_guarded)
    kickos_app_judge(faultsurvive_ovf tests/integration/check_faultsurvive.sh
      ARGS overflow ${KICKOS_ARCH} ${_fs_outcome})
  else()
    kickos_inapplicable(faultsurvive_ovf "memory not enforced, so no guard faults the overflow")
  endif()
  if(TARGET faultsurvive_off)
    if(_fs_unprivileged)
      kickos_app_judge(faultsurvive_off tests/integration/check_faultsurvive.sh
        ARGS offstack ${KICKOS_ARCH} ${_fs_outcome})
    else()
      kickos_inapplicable(faultsurvive_off "${_fs_privileged_why}")
    endif()
  endif()
  # Both enter through the syscall trap, whose entry contains a refused sp on either arch.
  if(TARGET faultsurvive_kwrite)
    if(_fs_unprivileged)
      kickos_app_judge(faultsurvive_kwrite tests/integration/check_faultsurvive.sh
        ARGS kwrite ${KICKOS_ARCH} contained)
      kickos_app_judge(faultsurvive_misalign tests/integration/check_faultsurvive.sh
        ARGS misalign ${KICKOS_ARCH} contained)
    else()
      kickos_inapplicable(faultsurvive_kwrite "${_fs_privileged_why}")
      kickos_inapplicable(faultsurvive_misalign "${_fs_privileged_why}")
    endif()
  endif()
  if(TARGET faultsurvive_lowedge)
    kickos_app_judge(faultsurvive_lowedge tests/integration/check_faultsurvive.sh
      ARGS lowedge ${KICKOS_ARCH} terminated)
  endif()
endif()

# The board set these arms cover. microbit has an emulator and is deliberately not in it.
if(NOT (KICKOS_QEMU_MPS2 OR KICKOS_ARCH STREQUAL "armv8a"
        OR KICKOS_BOARD STREQUAL "qemu-riscv" OR KICKOS_BOARD STREQUAL "qemu-riscv64"
        OR KICKOS_BOARD STREQUAL "qemu-x86_64" OR KICKOS_ARCH STREQUAL "sim"))
  return()
endif()

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME ${_tag}_faultsurvive
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
            "$<TARGET_FILE:faultsurvive>" survive ${KICKOS_ARCH} terminated)
  set_tests_properties(${_tag}_faultsurvive PROPERTIES TIMEOUT 30)
  add_test(NAME ${_tag}_faultsurvive_published
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_faultsurvive_pub.sh"
            "$<TARGET_FILE:faultsurvive_published>")
  set_tests_properties(${_tag}_faultsurvive_published PROPERTIES TIMEOUT 60)
  return()
endif()
kickos_add_qemu_test(TARGET faultsurvive
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
  ARGS survive ${KICKOS_ARCH} terminated)

if(_fs_guarded)
  kickos_add_qemu_test(NAME ${_tag}_faultoverflow TARGET faultsurvive_ovf
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
    ARGS overflow ${KICKOS_ARCH} ${_fs_outcome})
endif()
if(_fs_unprivileged AND TARGET faultsurvive_off)
  kickos_add_qemu_test(NAME ${_tag}_faultoffstack TARGET faultsurvive_off
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
    ARGS offstack ${KICKOS_ARCH} ${_fs_outcome})
endif()
if(_fs_unprivileged AND TARGET faultsurvive_kwrite)
  kickos_add_qemu_test(NAME ${_tag}_faultkwrite TARGET faultsurvive_kwrite
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
    ARGS kwrite ${KICKOS_ARCH} contained)
  kickos_add_qemu_test(NAME ${_tag}_faultmisalign TARGET faultsurvive_misalign
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
    ARGS misalign ${KICKOS_ARCH} contained)
endif()
if(TARGET faultsurvive_unread)
  kickos_add_qemu_test(NAME ${_tag}_faultunread TARGET faultsurvive_unread
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
    ARGS unread ${KICKOS_ARCH} terminated)
endif()
if(_fs_judged AND TARGET faultsurvive_lowedge)
  kickos_add_qemu_test(NAME ${_tag}_faultlowedge TARGET faultsurvive_lowedge
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
    ARGS lowedge ${KICKOS_ARCH} terminated)
endif()
