# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The i.MX RT1062 AIPSTZ slot pair (arch/arm/chip/imxrt1062/aipstz_gate.h) over a fake register
# file, and the chip's fixed MPU rows (arch/arm/chip/imxrt1062/mpu_rows.h) resolved per address.
# imxrt1062_mmap.h is forced ahead of every include: a host build generates no RT1062 chip
# header.
kickos_add_unit_test(NAME imxrtgate
  SOURCES aipstz_gate.cc mpu_rows.cc
  INCLUDES "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_SOURCE_DIR}/arch/arm/common" "${PROJECT_SOURCE_DIR}/arch/include")
target_compile_options(imxrtgate PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/imxrt1062_mmap.h")
# BEFORE: the sim's own kickos/arch/mpu_encoded.h would otherwise answer for the v7-M one.
target_include_directories(imxrtgate BEFORE PRIVATE "${PROJECT_SOURCE_DIR}/arch/arm/armv7m/include")
target_compile_definitions(imxrtgate PRIVATE KICKOS_ARM_REGS_FROM_SEAM=1 KICKOS_ARM_MPU_NONE=0
  KICKOS_ARM_MPU_PMSAV7=1 KICKOS_ARM_MPU_PMSAV8=2 KICKOS_ARM_MPU_SYSMPU=3 KICKOS_ARM_MPU=1)
