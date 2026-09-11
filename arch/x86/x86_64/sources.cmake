# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Included into arch/CMakeLists.txt's own directory scope: every source path below is
# relative to arch/, not to this directory, and the list order is the archive's member order.

# x86_64 in long mode, entered by UEFI with paging already enabled.
#
# NO common/arch_ram_common.cc: that file reads the chip linker script's __kickos_ram_start
# pair, and the image firmware loads here is a PE32+ application with no KickOS script at
# all. The RAM bounds are the FIRMWARE's answer, taken from the UEFI memory map, so the
# chip keeps its own copies.
#
# NO common/startup_ranges.cc either: the UEFI loader places .data with its initialised
# content and zero-fills the tail of a section whose virtual size exceeds its raw size, so
# there is no copy table and no zero table for a Reset_Handler to walk.
set(KICKOS_ARCH_SOURCES
  x86/x86_64/arch_x86_64.cc
  x86/x86_64/apic_x86_64.cc
  x86/x86_64/aspace_x86_64.cc
  x86/x86_64/desc_x86_64.cc
  x86/x86_64/ring3_x86_64.cc
  x86/x86_64/fault_x86_64.cc
  x86/x86_64/trap_x86_64.S
  x86/x86_64/switch.S
  x86/x86_64/panic_stack.S
  ${KICKOS_SEAM_DEFAULTS_COMMON})

# The fault report prints through the chip's COM1 primitives rather than through kprintf:
# the same reporter serves the kernel-free boot images, which link no kernel to print with.
set(KICKOS_ARCH_PRIVATE_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/x86/chip/${KICKOS_CHIP}/include")
