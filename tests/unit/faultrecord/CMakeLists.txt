# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The fault record's PC line over the real kernel/init/fault.cc.
kickos_add_unit_test(NAME fault_record_pc SOURCES fault_record.cc
  LIBRARIES kickos_kseam)
