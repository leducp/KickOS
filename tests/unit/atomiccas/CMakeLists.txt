# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# arch_cas_u32 on the forms a host can run: the sim's (x86_64's is the same instruction), the
# masked one every one-core kernel includes, over a fake interrupt state, and the bounded loop
# the arches whose store can be lost share, over a fake store attempt.
kickos_add_unit_test(NAME atomic_cas_sim SOURCES cas_contract.cc KERNEL TEST_PREFIX "sim.")
kickos_add_unit_test(NAME atomic_cas_masked SOURCES cas_contract.cc KERNEL TEST_PREFIX "masked."
  DEFINITIONS ATOMICCAS_MASKED=1)
kickos_add_unit_test(NAME atomic_cas_bounded SOURCES cas_contract.cc KERNEL
  TEST_PREFIX "bounded." DEFINITIONS ATOMICCAS_BOUNDED=1)
