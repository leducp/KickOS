# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The fault-attribution band: kickos_fault_below_stack and kickos_fault_stack_top,
# gated against the two rx72m silicon captures (faultsurvive_ovf, mpu_fault) that pin
# the band's contract without pinning its width.
kickos_add_unit_test(NAME fault_band SOURCES band.cc
  LIBRARIES kickos_kseam)
