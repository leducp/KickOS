# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Translating-backend opt-in: this chip's arch ships the arch_aspace_* family, and the frame
# pool the page tables come from is the part of the UEFI arena the kernel does not take at boot.
set(KICKOS_CHIP_TRANSLATES ON)
