# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The fault record's PC line over the real kernel/init/fault.cc, at the posture of a chip with
# stack limit registers, the only one whose record can carry no PC.
kickos_add_kseam(kickos_kseam_stklim POSTURE KICKOS_HAVE_STACK_LIMIT=1)
kickos_add_unit_test(NAME fault_record_pc SOURCES fault_record.cc
  LIBRARIES kickos_kseam_stklim)
