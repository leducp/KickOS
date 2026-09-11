# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Included into arch/CMakeLists.txt's own directory scope: every source path below is
# relative to arch/, not to this directory, and the list order is the archive's member order.

# RISC-V RV32IMAC (ESP32-C6 + QEMU virt).
set(KICKOS_ARCH_SOURCES
  riscv/rv32imac/arch_rv32imac.cc
  common/arch_ram_common.cc
  common/startup_ranges.cc
  ${KICKOS_SEAM_DEFAULTS_COMMON}
  riscv/rv32imac/arch_rv_ext_eoi_default.cc
  riscv/rv32imac/arch_rv_has_mcounteren_default.cc
  riscv/rv32imac/arch_rv_hw_mask_default.cc
  riscv/rv32imac/arch_rv_hw_unmask_default.cc
  riscv/rv32imac/arch_rv_inject_deliver_default.cc
  riscv/rv32imac/kickos_rv_ext_dispatch_dev_default.cc
  riscv/rv32imac/switch.S)
