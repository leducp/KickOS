# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Run by the custom commands that write the host tests' tables:
#   cmake -DSOURCE=<source> -DSTATE=<directory> [-DSCRIPT=<script.py>] -DFIXTURES=<request>,...
#         -P emit_fixture.cmake
# SCRIPT is emit_fixture.py beside this file unless given, and takes a scratch directory and the
# requests.

include("${SOURCE}/cmake/compose.cmake")
if(NOT DEFINED SCRIPT)
  set(SCRIPT "${CMAKE_CURRENT_LIST_DIR}/emit_fixture.py")
endif()
string(REPLACE "," ";" _fixtures "${FIXTURES}")
file(MAKE_DIRECTORY "${STATE}/tmp")
kickos_compose_python(_python "${SOURCE}/tools/compose" "${STATE}/venv" "${STATE}/tmp"
                      "${SOURCE}/tools/compose/tests")
execute_process(COMMAND ${_python} "${SCRIPT}" "${STATE}/work" ${_fixtures} RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "KickOS: ${SCRIPT} refused its tables (see above)")
endif()
