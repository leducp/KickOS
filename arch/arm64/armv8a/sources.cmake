# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Included into arch/CMakeLists.txt's own directory scope: every source path below is
# relative to arch/, not to this directory, and the list order is the archive's member order.

# ARMv8-A in AArch64 state (QEMU virt, Cortex-A53).
if(KICKOS_ARM64_GIC_VERSION EQUAL 2)
  if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
    message(FATAL_ERROR
      "KickOS: the own-image AMP posture is refused under ARM64_GIC_V2. A GICv2 doorbell "
      "targets CPU INTERFACE numbers, which the architecture ties to no identity register, "
      "so each core must publish its own and a peer's publication never reaches this "
      "image. What would fix it is a target number carried in the partition's shared "
      "region, published by each node before any peer sends. Build this partition under "
      "CONFIG_ARM64_GIC_V3, whose target is the affinity in MPIDR.")
  endif()
  set(_kos_gic_src arm64/common/arch_arm64_gicv2.cc)
elseif(KICKOS_ARM64_GIC_VERSION EQUAL 3)
  set(_kos_gic_src arm64/common/arch_arm64_gicv3.cc)
else()
  message(FATAL_ERROR
    "KICKOS_ARM64_GIC_VERSION is '${KICKOS_ARM64_GIC_VERSION}'; this arch implements "
    "2 and 3. The board states it through the Kconfig choice ARM64_GIC_V2 / ARM64_GIC_V3.")
endif()

# The GIC backend is the backend for arch_irq_line_kernel_owned (the doorbell's SGI) and for
# arch_irq_route (GICD_IROUTER on v3, GICD_ITARGETSR on v2), so those two fallbacks leave the
# list.
set(_armv8a_defaults ${KICKOS_SEAM_DEFAULTS_COMMON})
list(REMOVE_ITEM _armv8a_defaults
  common/arch_irq_line_kernel_owned_default.cc
  common/arch_irq_route_default.cc)

set(KICKOS_ARCH_SOURCES
  arm64/armv8a/arch_armv8a.cc
  arm64/armv8a/aspace_armv8a.cc
  arm64/armv8a/cache_armv8a.cc
  arm64/armv8a/vectors.S
  arm64/armv8a/switch.S
  arm64/armv8a/secondary.S
  # The cross-core kernel lock and the doorbell are one mechanism: the acquire loop services
  # a pending doorbell. Empty at one core.
  arm64/armv8a/klock_armv8a.cc
  common/doorbell_protocol.cc
  # A chip states the bases and the routing by defining this backend's map
  # (arm64/common/gicv2.h, gicv3.h).
  ${_kos_gic_src}
  # The A53 seams that name no device: the architected generic timer and the semihosting
  # dead end. No fallback for any of them sits in this archive; the definitions come from
  # the chip archive the group scans first.
  arm64/common/arch_arm64_a53.cc
  common/arch_ram_common.cc
  common/startup_ranges.cc
  ${_armv8a_defaults})
