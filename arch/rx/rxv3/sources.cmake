# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Included into arch/CMakeLists.txt's own directory scope: every source path below is
# relative to arch/, not to this directory, and the list order is the archive's member order.

# Renesas RX72M (RXv3).
set(KICKOS_ARCH_SOURCES
  rx/rxv3/arch_rxv3.cc
  rx/rxv3/emutls.cc
  common/arch_ram_common.cc
  common/startup_ranges.cc
  ${KICKOS_SEAM_DEFAULTS_COMMON}
  rx/rxv3/kickos_rx_dev_dispatch_default.cc
  rx/rxv3/kickos_rx_group_arm_default.cc
  rx/rxv3/switch.S)

set(KICKOS_ARCH_PRIVATE_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include") # console_tx.h (buffered console)
