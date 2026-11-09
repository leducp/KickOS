# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Test IRQ binding ownership through the real IRQ and capability-close code.
# The existing single-core fixture is sufficient; placement is not involved.
kickos_add_unit_test(NAME irq_bind_own SOURCES bind_owner.cc
  LIBRARIES kickos_kseam)

# A released binding's charge above one kernel core, at a posture where one binding and one
# notification are the whole budget.
kickos_add_kseam(kickos_kseam_irq_smp
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_SEAM_BUDGET_IRQ_BINDINGS=1 KICKOS_SEAM_BUDGET_NOTIFICATIONS=1)
kickos_add_unit_test(NAME irq_retire_charge SOURCES retire_charge.cc
  LIBRARIES kickos_kseam_irq_smp)
kickos_add_unit_test(NAME irq_retire_wait SOURCES retire_wait.cc
  LIBRARIES kickos_kseam_irq_smp)
