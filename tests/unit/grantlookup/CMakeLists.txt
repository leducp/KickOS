# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The lookups of <kickos/sys.h> against tables the tool emits from the golden compositions,
# one test program per table since each defines kickos_table. user/src/grant_lookup.cc and its
# neighbours are COMPILED IN over the fakes of lookup_seam.cc, never linked from kickos_user,
# whose syscall stubs would satisfy the kos_* names instead.
#
# HOST-ONLY: lookup_seam.cc defines public kos_* names.

set(_fixtures arm64 x86 xmc)
set(_requests "")
set(_tables "")
foreach(_fixture IN LISTS _fixtures)
  list(APPEND _requests "${_fixture}=${CMAKE_CURRENT_BINARY_DIR}/table_${_fixture}.c")
  list(APPEND _tables "${CMAKE_CURRENT_BINARY_DIR}/table_${_fixture}.c")
endforeach()
list(JOIN _requests "," _requests)
kickos_compose_inputs(_tool "${PROJECT_SOURCE_DIR}/tools/compose")
file(GLOB_RECURSE _platform CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/platform/*.yaml")
file(GLOB _systems CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/examples/composition/systems/*.yaml")
add_custom_command(
  OUTPUT ${_tables}
  COMMAND "${CMAKE_COMMAND}" -DSOURCE=${PROJECT_SOURCE_DIR}
          -DSTATE=${CMAKE_CURRENT_BINARY_DIR}/compose "-DFIXTURES=${_requests}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/emit_fixture.cmake"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/emit_fixture.cmake" "${CMAKE_CURRENT_SOURCE_DIR}/emit_fixture.py"
          "${PROJECT_SOURCE_DIR}/tools/compose/tests/test_arms.py" ${_tool} ${_platform} ${_systems}
  COMMENT "Emitting the lookup tests' tables"
  VERBATIM)

foreach(_fixture IN LISTS _fixtures)
  kickos_add_unit_test(NAME grant_lookup_${_fixture}
    SOURCES lookup_${_fixture}.cc lookup_seam.cc "${CMAKE_CURRENT_BINARY_DIR}/table_${_fixture}.c"
            "${PROJECT_SOURCE_DIR}/user/src/grant_lookup.cc"
            "${PROJECT_SOURCE_DIR}/user/src/init_status.cc"
            "${PROJECT_SOURCE_DIR}/user/src/task_trampoline.cc"
    INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
             "${PROJECT_BINARY_DIR}/generated/include")
endforeach()
