# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# MPU-enforcement opt-in for the QEMU mps2 chip. mps2.ld (and the qemu-m33 board
# override beside it) carves the KICKOS_HAVE_MPU .appdata window, so
# arch_domain_static_regions grants a real app-data region instead of reading the
# WEAK __kickos_appdata_* symbols as 0.
#
# Which PMSA revision is in play is a BOARD fact here, not a chip fact: unusually,
# because one chip backend serves several FPGA images. `qemu`/`qemu-m7`/`qemu-m3`
# (mps2-an386/an500/an385, M4/M7/M3) are PMSAv7 and use the shared armv7m
# apply/commit; `qemu-m33` (mps2-an505, Cortex-M33) is PMSAv8 and cannot, so it pulls
# the dedicated backend whose strong kickos_arch_mpu_commit + arch_mpu_region_encodable
# replace the v7-M fallback TUs. See docs/design-rp2350-mpu-armv8m.md.
#
# The top CMakeLists fail-loud floor includes this file in its own scope, so a plain
# set (no PARENT_SCOPE) is what it reads. KICKOS_ARM_PMSAV8_SOURCE is likewise read
# by arch/CMakeLists.txt (inherited into the add_subdirectory child scope) and, when
# KICKOS_HAVE_MPU is on, appended to the chip library sources.
#
# Validation status of these ports: see docs/reference/boards.md.
set(KICKOS_CHIP_ENFORCES_MPU ON)
if(KICKOS_BOARD STREQUAL "qemu-m33")
  set(KICKOS_ARM_PMSAV8_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../common/arch_arm_pmsav8.cc")
  # The chip TU must both CALL kickos_arm_pmsav8_init (MAIR + MEMFAULTENA) and, by
  # referencing it, anchor the PMSAv8 archive member into the link; an unreferenced
  # member would never be pulled and the v7-M commit fallback would answer instead.
  add_compile_definitions(KICKOS_MPS2_PMSAV8=1)
endif()
