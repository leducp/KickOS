# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Host arms for the per-address-space virtual range list. It is validation data over a
# granule, so it needs no page tables and no board, and the arms that matter most are the
# refusals: a range list that admits one byte past an entry hands the kernel a pointer the
# tables will fault on.
#
# Takes the kernel target's include path and definitions verbatim, as frame does: this TU
# compiles a kernel source, and a divergent config here would gate a different translation
# unit than the one that ships.
kickos_add_unit_test(NAME vrange_list
  SOURCES vrange_list.cc "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
