# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Included into arch/CMakeLists.txt's own directory scope: every source path below is
# relative to arch/, not to this directory, and the list order is the archive's member order.

# sim.cc is the backend for arch_console_write_sync, arch_console_tx_backend,
# arch_console_reclaim_window, kfault_terminate and arch_periph_reg_write, so those fallbacks
# are dropped.
# SystemCoreClock has no chip to define it here.
set(_sim_defaults ${KICKOS_SEAM_DEFAULTS_COMMON})
list(REMOVE_ITEM _sim_defaults
  common/arch_console_tx_backend_default.cc
  common/arch_console_reclaim_window_default.cc
  common/arch_console_write_sync_default.cc
  common/arch_periph_reg_write_default.cc
  common/kfault_terminate_default.cc)

set(KICKOS_ARCH_HOSTED ON)
set(KICKOS_ARCH_SOURCES
  sim/sim.cc
  sim/start.cc
  sim/system_core_clock_default.cc
  ${_sim_defaults})
set(KICKOS_ARCH_PRIVATE_INCLUDE_DIRS
  # The arch-neutral cpp constants (fatal_status.ld.h).
  "${CMAKE_CURRENT_SOURCE_DIR}/common"
  # Telemetry: the sim reads lib's rtt.h to flush the RTT ch1 ring to a file at shutdown.
  "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include")
if(KICKOS_MULTI_INSTANCE)
  # Linked PUBLIC: -pthread has to reach the final link of every app, not just this
  # archive's compiles.
  find_package(Threads REQUIRED)
  set(KICKOS_ARCH_LINK_LIBRARIES Threads::Threads)
endif()
