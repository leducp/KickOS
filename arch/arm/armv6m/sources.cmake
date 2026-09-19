# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Included into arch/CMakeLists.txt's own directory scope: every source path below is
# relative to arch/, not to this directory, and the list order is the archive's member order.

# Cortex-M0/M0+ (RP2040). No arch_trace_now fallback: v6-M has no DWT, so the trace clock
# is a chip contract.
set(_armv6m_fault "")
if(KICKOS_FAULT_ISOLATION)
  set(_armv6m_fault arm/armv6m/arch_armv6m_fault.cc)
endif()

set(KICKOS_ARCH_SOURCES
  arm/armv6m/arch_armv6m.cc
  ${_armv6m_fault}
  arm/common/arch_arm_common.cc
  arm/common/arch_arm_mpu_pending.cc
  common/arch_ram_common.cc
  common/startup_ranges.cc
  ${KICKOS_SEAM_DEFAULTS_COMMON}
  ${KICKOS_SEAM_DEFAULTS_ARM}
  arm/read_tp.S
  arm/armv6m/switch.S)

# Resolves the <arm_isa.h> arm/common/arch_arm_common.cc includes. PRIVATE and a directory
# of its own: it must not reach an installed consumer, and a directory holding a second
# regs.h would shadow the one a common TU means.
set(KICKOS_ARCH_PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/arm/armv6m/isa")
