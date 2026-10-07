# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The real kernel/domain/domain.cc: its handle codec and its RAM admission. What no case reaches
# is dropped at link, so the suite carries a seam only for what domain_for calls.
kickos_add_unit_test(NAME domain_unit
  SOURCES domain_unit.cc
    "${PROJECT_SOURCE_DIR}/kernel/domain/domain.cc"
    "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
target_compile_options(domain_unit PRIVATE -ffunction-sections -fdata-sections)
target_link_options(domain_unit PRIVATE -Wl,--gc-sections)
