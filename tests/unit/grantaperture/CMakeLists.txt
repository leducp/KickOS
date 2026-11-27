# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The REAL kernel/grant/grant.cc over each chip's own chip_limits.h and chip_tables.h, written
# from its chip file for one arch and included ahead of everything else with that view's cases:
# the host build's own are the sim's. A third field names a view other than the chip's only one.
# The code-map answers are the selftest's own (code_map.cmake).
set(_tool "${PROJECT_SOURCE_DIR}/tools/compose")
kickos_compose_inputs(_tool_inputs "${_tool}")
include("${PROJECT_SOURCE_DIR}/user/apps/common/selftest/code_map.cmake")
foreach(_pair IN ITEMS "rp2040:armv6m" "rp2350:armv7m" "esp32c6:rv32imac" "stm32f411:armv7m"
                       "mk64f:armv7m" "sam3x8e:armv7m" "xmc4800:armv7m" "rx72m:rxv3" "imxrt1062:armv7m"
                       "imx8mp:armv7m" "imx8mp:armv8a:imx8mp_a53" "virt_rv32:rv32imac" "mps2:armv7m"
                       "an505:armv7m")
  string(REPLACE ":" ";" _pair "${_pair}")
  list(GET _pair 0 _chip)
  list(GET _pair 1 _arch)
  set(_view "${_chip}")
  list(LENGTH _pair _fields)
  if(_fields EQUAL 3)
    list(GET _pair 2 _view)
  endif()
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/${_view}")
  set(_limits "${_gen}/include/kickos/chip_limits.h")
  set(_tables "${_gen}/chip/chip_tables.h")
  kickos_compose_python(_python "${_tool}" "${_gen}/venv" "${_gen}/tmp")
  add_custom_command(
    OUTPUT "${_limits}" "${_tables}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}/tmp"
    COMMAND ${_python} -m kickos_compose chip "${PROJECT_SOURCE_DIR}/platform/${_chip}/chip.yaml" --arch ${_arch}
            --include-dir "${_gen}/include" --chip-dir "${_gen}/chip"
    DEPENDS "${PROJECT_SOURCE_DIR}/platform/${_chip}/chip.yaml" ${_tool_inputs}
    COMMENT "Writing the ${_chip} chip tables for grant_aperture_${_view}"
    VERBATIM)
  set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${_gen}")
  set(_main "${_gen}/aperture_${_view}.cc")
  file(CONFIGURE OUTPUT "${_main}" CONTENT "#include \"${_limits}\"
#include \"${_tables}\"
#include \"${CMAKE_CURRENT_SOURCE_DIR}/cases/${_view}.h\"
#include \"${CMAKE_CURRENT_SOURCE_DIR}/aperture.cc\"
")
  set_source_files_properties("${_main}" PROPERTIES OBJECT_DEPENDS "${_limits};${_tables}")
  kickos_add_unit_test(NAME grant_aperture_${_view} TEST_PREFIX ${_view}.
    SOURCES "${_main}" "${_limits}" "${_tables}"
            "${PROJECT_SOURCE_DIR}/tests/unit/common/kpanic_seam.cc"
            "${PROJECT_SOURCE_DIR}/tests/unit/common/region_seam.cc"
            "${PROJECT_SOURCE_DIR}/kernel/grant/grant.cc"
    INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
             "${PROJECT_SOURCE_DIR}/tests/unit/common"
    DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
  kickos_code_map("${_view}" _blocks _answers)
  if(_blocks)
    target_compile_definitions(grant_aperture_${_view} PRIVATE "GRANT_CODE_MAP_BLOCKS=${_blocks}"
                                                               "GRANT_CODE_MAP_ANSWERS=${_answers}")
  endif()
  target_compile_options(grant_aperture_${_view} PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(grant_aperture_${_view} PRIVATE -Wl,--gc-sections)
endforeach()
