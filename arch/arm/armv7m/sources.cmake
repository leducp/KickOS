# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Included into arch/CMakeLists.txt's own directory scope: every source path below is
# relative to arch/, not to this directory, and the list order is the archive's member order.

set(KICKOS_ARCH_SOURCES
  arm/armv7m/arch_armv7m.cc
  arm/armv7m/cache.cc
  arm/common/arch_arm_common.cc
  arm/common/arch_arm_mpu_pending.cc
  common/arch_ram_common.cc
  common/startup_ranges.cc
  ${KICKOS_SEAM_DEFAULTS_COMMON}
  ${KICKOS_SEAM_DEFAULTS_ARM}
  arm/armv7m/arch_fault_report_extra_default.cc
  arm/armv7m/arch_trace_now_default.cc
  arm/read_tp.S
  arm/armv7m/switch.S)

# The DWT arch_trace_now fallback runs on the PendSV-tail emit path, so its TU (and
# arch_armv7m.cc, which carries the rest of that path) must be FP-register-free.
if(KICKOS_TELEMETRY_ON)
  set_source_files_properties(arm/armv7m/arch_armv7m.cc
                              arm/armv7m/arch_trace_now_default.cc
    PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()

# Resolves the <arm_isa.h> arm/common/arch_arm_common.cc includes. PRIVATE and a directory
# of its own: it must not reach an installed consumer, and a directory holding a second
# regs.h would shadow the one a common TU means.
set(KICKOS_ARCH_PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/arm/armv7m/isa")
