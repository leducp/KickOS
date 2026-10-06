# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_chip_generate(<description> <arch>)
#   Runs `kickos_compose chip` at configure on <description>, a board file or the chip file it
#   names, for the cluster whose architecture is <arch>. It writes kickos/chip_mmap.h and
#   kickos/chip_limits.h under ${PROJECT_BINARY_DIR}/generated/include, and irq.h, chip_layout.h,
#   chip_tables.h and chip.cmake under ${PROJECT_BINARY_DIR}/generated/chip, each only when its
#   bytes change. The description, the chip file beside it and the tool are configure
#   dependencies. A refusal fails the configure with the tool's `<file>:<line>: <rule>: <message>`
#   lines. Requires uv on PATH, as kickos_compose() does.
function(kickos_chip_generate description arch)
  set(_tool "${PROJECT_SOURCE_DIR}/tools/compose")
  get_filename_component(_platform "${description}" DIRECTORY)
  file(GLOB _tool_sources CONFIGURE_DEPENDS "${_tool}/kickos_compose/*.py")
  set(_inputs "${description}" "${_platform}/chip.yaml" ${_tool_sources} "${_tool}/pyproject.toml"
              "${_tool}/uv.lock")
  list(REMOVE_DUPLICATES _inputs)
  set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})
  set(_hashes "${arch}\n")
  foreach(_input IN LISTS _inputs)
    file(SHA256 "${_input}" _input_hash)
    string(APPEND _hashes "${_input} ${_input_hash}\n")
  endforeach()
  string(SHA256 _hash "${_hashes}")

  set(_include "${PROJECT_BINARY_DIR}/generated/include")
  set(_chip "${PROJECT_BINARY_DIR}/generated/chip")
  set(_state "${PROJECT_BINARY_DIR}/kickos_compose/chip")
  set(_outputs "${_include}/kickos/chip_mmap.h" "${_include}/kickos/chip_limits.h" "${_chip}/irq.h"
               "${_chip}/chip_layout.h" "${_chip}/chip_tables.h" "${_chip}/chip.cmake")
  set(_recorded "")
  if(EXISTS "${_state}/inputs.sha256")
    file(READ "${_state}/inputs.sha256" _recorded)
  endif()
  set(_stale FALSE)
  if(NOT _recorded STREQUAL _hash)
    set(_stale TRUE)
  endif()
  foreach(_output IN LISTS _outputs)
    if(NOT EXISTS "${_output}")
      set(_stale TRUE)
    endif()
  endforeach()
  if(NOT _stale)
    return()
  endif()

  find_program(KICKOS_UV uv)
  if(NOT KICKOS_UV)
    message(FATAL_ERROR "KickOS: uv not found on PATH; the chip headers are written by tools/compose, which "
      "runs under uv (https://docs.astral.sh/uv/)")
  endif()
  file(REMOVE "${_state}/inputs.sha256")
  file(MAKE_DIRECTORY "${_state}/tmp")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "UV_PROJECT_ENVIRONMENT=${CMAKE_BINARY_DIR}/kickos_compose/venv"
            UV_PYTHON_DOWNLOADS=never "PYTHONPATH=${_tool}" PYTHONDONTWRITEBYTECODE=1
            "TMPDIR=${_state}/tmp"
            "${KICKOS_UV}" run --project "${_tool}" --locked --quiet
            python -m kickos_compose chip "${description}" --arch "${arch}"
            --include-dir "${_include}" --chip-dir "${_chip}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err
    TIMEOUT 600)
  string(STRIP "${_out}\n${_err}" _said)
  string(REPLACE "\n" "\n  " _said "${_said}")
  if(_rc STREQUAL "3")
    message(FATAL_ERROR "KickOS: the host tool refused ${description}:\n  ${_said}")
  elseif(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "KickOS: could not write the chip headers from ${description} (uv and Python >= 3.12 "
      "required, with the ruamel.yaml its uv.lock pins) (${_rc}):\n  ${_said}")
  endif()
  file(WRITE "${_state}/inputs.sha256" "${_hash}")
  message(STATUS "KickOS: chip headers written from ${description}")
endfunction()
