# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Run by the target kickos_admit_default_composition (cmake/manifest.cmake) defines:
#   cmake -DSOURCE=<source> -DBUILD=<build> -DCOMPOSITION=<file> -DMANIFEST=<file> -DSTAMP=<file>
#         -P admit_default.cmake
# Its environment is tests/static/check_platform.sh's, so a run writes nothing into the source tree.

find_program(_uv uv)
if(NOT _uv)
  message(FATAL_ERROR "KickOS: uv not found on PATH; the build admits ${COMPOSITION} with "
                      "tools/compose, which runs under uv (https://docs.astral.sh/uv/)")
endif()

set(_state "${BUILD}/compose/admit")
file(MAKE_DIRECTORY "${_state}/tmp")
set(ENV{UV_PROJECT_ENVIRONMENT} "${_state}/venv")
set(ENV{UV_PYTHON_DOWNLOADS} never)
set(ENV{PYTHONPATH} "${SOURCE}/tools/compose")
set(ENV{PYTHONDONTWRITEBYTECODE} 1)
set(ENV{TMPDIR} "${_state}/tmp")

file(REMOVE "${STAMP}")
execute_process(
  COMMAND "${_uv}" run --project "${SOURCE}/tools/compose" --locked --quiet
          python -m kickos_compose admit "${COMPOSITION}" --manifest "${MANIFEST}"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "KickOS: ${COMPOSITION} is refused against ${MANIFEST} (see above)")
endif()
file(TOUCH "${STAMP}")
