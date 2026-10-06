# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The own-image AMP console claims of virt_arm64 and the RP2350, each played against a fake
# partition word, clock and spinlock. Each source defines the primitives its claim header
# declares.
kickos_add_unit_test(NAME amp_console_claim
  SOURCES virt_arm64_claim.cc rp2350_claim.cc
  KERNEL
  INCLUDES "${PROJECT_SOURCE_DIR}")
