# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding the `faultsurvive` family: one image per fault mode, each asking that the
# violation be contained and the rest of the system carry on. Each mode has a target of its own,
# so a clause here is keyed on the same posture that built its image.

if(NOT TARGET faultsurvive)
  return()
endif()

# The board set these arms cover. microbit has an emulator and is deliberately not in it.
if(NOT (KICKOS_CHIP STREQUAL "mps2" OR KICKOS_ARCH STREQUAL "armv8a"
        OR KICKOS_BOARD STREQUAL "qemu-riscv" OR KICKOS_BOARD STREQUAL "qemu-riscv64"
        OR KICKOS_BOARD STREQUAL "qemu-x86_64" OR KICKOS_ARCH STREQUAL "sim"))
  return()
endif()

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME ${_tag}_faultsurvive
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
            "$<TARGET_FILE:faultsurvive>" survive ${KICKOS_ARCH} terminated)
  set_tests_properties(${_tag}_faultsurvive PROPERTIES TIMEOUT 30)
  # The gate builds its own tree, one service-list provider linking per image; the
  # services_none guard keeps it from registering inside the tree it configures and recursing.
  if(KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
    add_test(NAME ${_tag}_faultsurvive_published
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_faultsurvive_pub.sh"
              "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
    set_tests_properties(${_tag}_faultsurvive_published PROPERTIES TIMEOUT 300)
  endif()
else()
  kickos_add_qemu_test(TARGET faultsurvive
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
    ARGS survive ${KICKOS_ARCH} terminated)
endif()

set(_fs_outcome terminated)
if(KICKOS_ARCH STREQUAL "rv32imac")
  set(_fs_outcome contained)
endif()

if(KICKOS_ARCH STREQUAL "sim" OR NOT KICKOS_HAVE_MPU)
  return()
endif()
kickos_add_qemu_test(NAME ${_tag}_faultoverflow TARGET faultsurvive_ovf
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
  ARGS overflow ${KICKOS_ARCH} ${_fs_outcome})
kickos_add_qemu_test(NAME ${_tag}_faultoffstack TARGET faultsurvive_off
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
  ARGS offstack ${KICKOS_ARCH} ${_fs_outcome})

if(KICKOS_ARCH STREQUAL "rv32imac")
  kickos_add_qemu_test(NAME ${_tag}_faultkwrite TARGET faultsurvive_kwrite
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
    ARGS kwrite ${KICKOS_ARCH} ${_fs_outcome})
  kickos_add_qemu_test(NAME ${_tag}_faultmisalign TARGET faultsurvive_misalign
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
    ARGS misalign ${KICKOS_ARCH} ${_fs_outcome})
endif()

if(KICKOS_ARCH STREQUAL "rv32imac")
  kickos_add_qemu_test(NAME ${_tag}_faultlowedge TARGET faultsurvive_lowedge
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_faultsurvive.sh"
    ARGS lowedge ${KICKOS_ARCH} terminated)
endif()
