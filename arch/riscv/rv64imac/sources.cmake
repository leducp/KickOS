# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Included into arch/CMakeLists.txt's own directory scope: every source path below is
# relative to arch/, not to this directory, and the list order is the archive's member order.

# RISC-V RV64IMAC (QEMU virt). libgcc_rv64imac.cc holds the two compiler-runtime helpers
# kernel text calls, which the split image cannot take from the multilib in the app's half.
set(KICKOS_ARCH_SOURCES
  riscv/rv64imac/arch_rv64imac.cc
  riscv/rv64imac/aspace_rv64imac.cc
  riscv/rv64imac/klock_rv64imac.cc
  common/doorbell_protocol.cc
  riscv/rv64imac/libgcc_rv64imac.cc
  riscv/rv64imac/switch.S
  riscv/rv64imac/trap.S
  common/arch_ram_common.cc
  common/startup_ranges.cc
  ${KICKOS_SEAM_DEFAULTS_COMMON})
