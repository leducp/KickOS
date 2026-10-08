# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The RAM region rule's two copies over one table: arch_ram_region_size and arch_ram_region_align,
# and the composition tool's (tools/compose/kickos_compose/region.py, evaluated by emit_align.py),
# which the configure also asks for the boot arena's figures. The C copy is compiled once per
# stride posture, and each program checks the rows of its own stride. The strided posture, where
# the thread pointer is SP masked down to the stride, is one no host build is, so it is set here.
set(_ra_table "${CMAKE_CURRENT_BINARY_DIR}/align_table.h")
file(GLOB _ra_tool CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tools/compose/kickos_compose/*.py")
add_custom_command(
  OUTPUT "${_ra_table}"
  COMMAND "${CMAKE_COMMAND}" -DSOURCE=${PROJECT_SOURCE_DIR}
          -DSTATE=${CMAKE_CURRENT_BINARY_DIR}/compose
          "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/emit_align.py"
          "-DFIXTURES=${_ra_table}"
          -P "${PROJECT_SOURCE_DIR}/tests/unit/grantlookup/emit_fixture.cmake"
  DEPENDS "${PROJECT_SOURCE_DIR}/tests/unit/grantlookup/emit_fixture.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/emit_align.py" ${_ra_tool}
          "${PROJECT_SOURCE_DIR}/tools/compose/pyproject.toml"
          "${PROJECT_SOURCE_DIR}/tools/compose/uv.lock"
  COMMENT "Evaluating the composition tool's RAM region rule over the region table"
  VERBATIM)

kickos_add_unit_test(NAME ram_align_copies TEST_PREFIX stride_
  SOURCES align_copies.cc "${_ra_table}"
  INCLUDES "${CMAKE_CURRENT_BINARY_DIR}"
  POSTURE KICKOS_TLS=1 KICKOS_TLS_FROM_SP=1 KICKOS_TLS_STRIDE=8192 KICKOS_TLS_STRIDE_LOG2=13)
kickos_add_unit_test(NAME ram_align_copies_unstrided TEST_PREFIX unstrided_
  SOURCES align_copies.cc "${_ra_table}"
  INCLUDES "${CMAKE_CURRENT_BINARY_DIR}"
  POSTURE KICKOS_TLS=0)
