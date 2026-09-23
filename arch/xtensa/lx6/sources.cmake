# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Included into arch/CMakeLists.txt's own directory scope: every source path below is
# relative to arch/, not to this directory, and the list order is the archive's member order.

# ESP32 Xtensa LX6. arch_xtensa.cc is the backend for arch_irq_line_core, and above one kernel
# core for arch_irq_route, so those fallbacks leave the list.
set(_lx6_defaults ${KICKOS_SEAM_DEFAULTS_COMMON})
list(REMOVE_ITEM _lx6_defaults common/arch_irq_line_core_default.cc)
if(KICKOS_KERNEL_CORES GREATER 1)
  list(REMOVE_ITEM _lx6_defaults common/arch_irq_route_default.cc)
endif()

set(KICKOS_ARCH_SOURCES
  xtensa/lx6/arch_xtensa.cc
  xtensa/lx6/klock_lx6.cc
  common/doorbell_protocol.cc
  common/arch_ram_common.cc
  common/startup_ranges.cc
  ${_lx6_defaults}
  xtensa/lx6/arch_clock_now_default.cc
  xtensa/lx6/arch_trace_now_default.cc
  xtensa/lx6/switch.S)
