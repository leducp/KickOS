# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Test IRQ binding ownership through the real IRQ and capability-close code.
# The existing single-core fixture is sufficient; placement is not involved.
kickos_add_unit_test(NAME irq_bind_own SOURCES bind_owner.cc
  LIBRARIES kickos_kseam)
