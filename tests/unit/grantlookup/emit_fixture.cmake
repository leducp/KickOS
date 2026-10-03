# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Run by the custom commands that write the host tests' tables:
#   cmake -DSOURCE=<source> -DSTATE=<directory> [-DSCRIPT=<script.py>] -DFIXTURES=<request>,...
#         -P emit_fixture.cmake
# SCRIPT is emit_fixture.py beside this file unless given, and takes a scratch directory and the
# requests. Its environment is tests/static/check_platform.sh's, so a run writes nothing into the
# source tree.

find_program(_uv uv)
if(NOT _uv)
  message(FATAL_ERROR "KickOS: uv not found on PATH; the host tests' tables are emitted by "
                      "tools/compose, which runs under uv (https://docs.astral.sh/uv/)")
endif()

if(NOT DEFINED SCRIPT)
  set(SCRIPT "${CMAKE_CURRENT_LIST_DIR}/emit_fixture.py")
endif()
string(REPLACE "," ";" _fixtures "${FIXTURES}")
file(MAKE_DIRECTORY "${STATE}/tmp")
set(ENV{UV_PROJECT_ENVIRONMENT} "${STATE}/venv")
set(ENV{UV_PYTHON_DOWNLOADS} never)
set(ENV{PYTHONPATH} "${SOURCE}/tools/compose:${SOURCE}/tools/compose/tests")
set(ENV{PYTHONDONTWRITEBYTECODE} 1)
set(ENV{TMPDIR} "${STATE}/tmp")

execute_process(
  COMMAND "${_uv}" run --project "${SOURCE}/tools/compose" --locked --quiet
          python "${SCRIPT}" "${STATE}/work" ${_fixtures}
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "KickOS: ${SCRIPT} refused its tables (see above)")
endif()
