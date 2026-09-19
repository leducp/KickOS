# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The PMSAv7 descriptor writer and the stash every ARM backend commits from, compiled for the
# host, with the register window, the barriers and the interrupt bracket coming from
# arm_regs_seam.cc (KICKOS_ARM_REGS_FROM_SEAM). This gate compiles the backends' own
# translation units; a moved file would leave it witnessing nothing.
set(_mpuskip_backend "${PROJECT_SOURCE_DIR}/arch/arm/common/arch_arm_mpu_pmsav7.cc")
set(_mpuskip_pending "${PROJECT_SOURCE_DIR}/arch/arm/common/arch_arm_mpu_pending.cc")
foreach(_src "${_mpuskip_backend}" "${_mpuskip_pending}")
  if(NOT EXISTS "${_src}")
    message(FATAL_ERROR
      "KickOS: ${_src} is gone, so mpu_skip would witness nothing. This gate compiles the "
      "PMSAv7 descriptor writer and the ARM pending-stash member's own translation units.")
  endif()
endforeach()

kickos_add_kernel_host_lib(kickos_arm_mpu_pmsav7_host
  SOURCES "${_mpuskip_backend}" "${_mpuskip_pending}" arm_regs_seam.cc
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/arch/arm/common"
  POSTURE KICKOS_HAVE_MPU=1 KICKOS_NUM_CORES=1 KICKOS_ARM_MPU_NONE=0
          KICKOS_ARM_MPU_PMSAV7=1 KICKOS_ARM_MPU_PMSAV8=2 KICKOS_ARM_MPU_SYSMPU=3
          KICKOS_ARM_MPU=1 KICKOS_ARM_REGS_FROM_SEAM=1)
# BEFORE, and this is the whole reason the gate configures: the sim carries an
# arch/sim/include/kickos/arch/mpu_encoded.h of its own, and the backend must see the v7-M
# one or it compiles against a struct with no descriptor words in it.
target_include_directories(kickos_arm_mpu_pmsav7_host BEFORE PUBLIC
  "${PROJECT_SOURCE_DIR}/arch/arm/armv7m/include")

kickos_add_unit_test(NAME mpu_skip SOURCES mpu_skip.cc mpu_pending.cc
  LIBRARIES kickos_arm_mpu_pmsav7_host)
