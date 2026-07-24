# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Chip-NEUTRAL userspace client libraries: they speak only ABI (no chip register, no
# MMIO), so they build on every chip and sit outside any per-chip gate.
add_subdirectory(spi_client)
