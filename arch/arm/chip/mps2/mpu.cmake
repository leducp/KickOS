# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# MPU-enforcement opt-in for the QEMU mps2 chip. mps2.ld carves the KICKOS_HAVE_MPU
# .appdata window, so arch_domain_static_regions grants a real app-data region instead
# of reading the WEAK __kickos_appdata_* symbols as 0.
#
# There is nothing chip-specific to pull in: the an386 image is a Cortex-M4, so the
# shared armv7m PMSAv7 arch_mpu_apply/commit is the entire backend. What makes this
# file worth having anyway is that, alone among the enforcing ARM chips, this one is
# RUNNABLE -- so `--preset qemu -DKICKOS_HAVE_MPU=1` is a real runtime enforcement gate
# (the suite runs unprivileged, mpu_fault takes an actual MemManage denial) and not
# just the link check that build-boards-mpu can give for a silicon part.
#
# Without this file the window in the .ld would exist and still be unreachable: the
# fail-loud floor in the top CMakeLists rejects KICKOS_HAVE_MPU on a chip that ships no
# mpu.cmake, precisely so enforcement can never be a silent no-op. It includes this
# file in its own scope, so a plain set (no PARENT_SCOPE) is what it reads.
#
# Validation status of this port: see docs/reference/boards.md.
set(KICKOS_CHIP_ENFORCES_MPU ON)
