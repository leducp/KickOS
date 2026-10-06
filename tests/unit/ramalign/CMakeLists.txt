# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# arch_ram_region_align where the thread pointer is SP masked down to the stride, which no host
# build is: the posture is set here, and the region seam is the fixture's.
kickos_add_unit_test(NAME ram_align
  SOURCES stride_align.cc
  POSTURE KICKOS_TLS=1 KICKOS_TLS_FROM_SP=1 KICKOS_TLS_STRIDE=8192 KICKOS_TLS_STRIDE_LOG2=13)

# The align rule's three copies over one table: arch_ram_region_align, kickos_region_align
# (cmake/boot_arena.cmake, evaluated here) and the composition tool's ram_align
# (tools/compose/kickos_compose/supply.py, evaluated by emit_align.py). The C copy is compiled
# once per stride posture, and each program checks the rows of its own stride.
set(_ra_strides 0 8192)
set(_ra_geometries 0:0 32:0 64:0 256:0 32:1 256:1)
set(_ra_wants 1 16 31 96 100 512 1000 4096 4097 8160 8172 8192 8193 8224 12000 16384)

function(_kickos_ra_align want mn pow2 stride out)
  set(KICKOS_TLS OFF)
  set(KICKOS_TLS_FROM_SP OFF)
  set(KICKOS_STACK_STRIDE "")
  if(NOT stride EQUAL 0)
    set(KICKOS_TLS ON)
    set(KICKOS_TLS_FROM_SP ON)
    set(KICKOS_STACK_STRIDE ${stride})
  endif()
  kickos_region_align("${want}" "${mn}" "${pow2}" _a)
  set(${out} "${_a}" PARENT_SCOPE)
endfunction()

set(_ra_cases "")
foreach(_stride IN LISTS _ra_strides)
  foreach(_geometry IN LISTS _ra_geometries)
    string(REPLACE ":" ";" _g "${_geometry}")
    list(GET _g 0 _mn)
    list(GET _g 1 _pow2)
    foreach(_want IN LISTS _ra_wants)
      _kickos_ra_align(${_want} ${_mn} ${_pow2} ${_stride} _align)
      string(APPEND _ra_cases "${_mn} ${_pow2} ${_stride} ${_want} ${_align}\n")
    endforeach()
  endforeach()
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/align_cases.txt" CONTENT "${_ra_cases}")

set(_ra_table "${CMAKE_CURRENT_BINARY_DIR}/align_table.h")
add_custom_command(
  OUTPUT "${_ra_table}"
  COMMAND "${CMAKE_COMMAND}" -DSOURCE=${PROJECT_SOURCE_DIR}
          -DSTATE=${CMAKE_CURRENT_BINARY_DIR}/compose
          "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/emit_align.py"
          "-DFIXTURES=${CMAKE_CURRENT_BINARY_DIR}/align_cases.txt,${_ra_table}"
          -P "${PROJECT_SOURCE_DIR}/tests/unit/grantlookup/emit_fixture.cmake"
  DEPENDS "${PROJECT_SOURCE_DIR}/tests/unit/grantlookup/emit_fixture.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/emit_align.py" "${CMAKE_CURRENT_BINARY_DIR}/align_cases.txt"
          "${PROJECT_SOURCE_DIR}/tools/compose/kickos_compose/supply.py"
          "${PROJECT_SOURCE_DIR}/tools/compose/kickos_compose/manifest.py"
          "${PROJECT_SOURCE_DIR}/tools/compose/pyproject.toml"
          "${PROJECT_SOURCE_DIR}/tools/compose/uv.lock"
  COMMENT "Evaluating the composition tool's align rule over the align table"
  VERBATIM)

kickos_add_unit_test(NAME ram_align_copies TEST_PREFIX stride_
  SOURCES align_copies.cc "${_ra_table}"
  INCLUDES "${CMAKE_CURRENT_BINARY_DIR}"
  POSTURE KICKOS_TLS=1 KICKOS_TLS_FROM_SP=1 KICKOS_TLS_STRIDE=8192 KICKOS_TLS_STRIDE_LOG2=13)
kickos_add_unit_test(NAME ram_align_copies_unstrided TEST_PREFIX unstrided_
  SOURCES align_copies.cc "${_ra_table}"
  INCLUDES "${CMAKE_CURRENT_BINARY_DIR}"
  POSTURE KICKOS_TLS=0)
