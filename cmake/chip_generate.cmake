# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_chip_generate(<description> <arch>)
#   Runs `kickos_compose chip` at configure on <description>, a board file or the chip file it
#   names, for the cluster whose architecture is <arch>. It writes kickos/chip_mmap.h and
#   kickos/chip_limits.h under ${PROJECT_BINARY_DIR}/generated/include, and irq.h, chip_layout.h,
#   chip_tables.h, chip.cmake, board_pins.h and board_buses.h under
#   ${PROJECT_BINARY_DIR}/generated/chip, each only when its bytes change. The description, the chip
#   file beside it and the tool are configure dependencies. A refusal fails the configure with the
#   tool's `<file>:<line>: <rule>: <message>` lines. Requires uv on PATH, as kickos_compose() does.
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
               "${_chip}/chip_layout.h" "${_chip}/chip_tables.h" "${_chip}/chip.cmake" "${_chip}/board_pins.h"
               "${_chip}/board_buses.h")
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
  elseif(_rc STREQUAL "4")
    message(FATAL_ERROR "KickOS: the host tool cannot write the chip headers from ${description} for "
      "arch ${arch}:\n  ${_said}")
  elseif(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "KickOS: could not write the chip headers from ${description} (uv and Python >= 3.12 "
      "required, with the ruamel.yaml its uv.lock pins) (${_rc}):\n  ${_said}")
  endif()
  file(WRITE "${_state}/inputs.sha256" "${_hash}")
  message(STATUS "KickOS: chip headers written from ${description}")
endfunction()

# kickos_board_undescribed(<out> <source_dir> <board> <chip>)
#   Empty when <board> has its board file beside <chip>'s chip file and a default composition
#   under <source_dir>, else what it lacks.
function(kickos_board_undescribed out source_dir board chip)
  set(_said "")
  if(chip STREQUAL "")
    set(_said "names no chip")
  elseif(NOT EXISTS "${source_dir}/platform/${chip}/chip.yaml")
    set(_said "names chip '${chip}', which has no chip file platform/${chip}/chip.yaml")
  elseif(NOT EXISTS "${source_dir}/platform/${chip}/${board}.yaml")
    set(_said "has no board file platform/${chip}/${board}.yaml")
  elseif(NOT EXISTS "${source_dir}/boards/${board}/composition.yaml")
    set(_said "has no default composition boards/${board}/composition.yaml, which "
              "KickOS::system_default is built from")
  endif()
  string(JOIN "" _said ${_said})
  set(${out} "${_said}" PARENT_SCOPE)
endfunction()

# kickos_chip_arm_backend(<out_backend> <out_source> <region_unit>)
#   The armv6m/armv7m MPU backend a chip file's region unit names, and the source it adds to the
#   chip archive, empty where it adds none.
function(kickos_chip_arm_backend out_backend out_source unit)
  set(_source "")
  if(unit STREQUAL "pmsav6" OR unit STREQUAL "pmsav7")
    set(_backend PMSAV7)
  elseif(unit STREQUAL "pmsav8")
    set(_backend PMSAV8)
    get_filename_component(_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../arch/arm/common/arch_arm_pmsav8.cc"
                           ABSOLUTE)
  elseif(unit STREQUAL "sysmpu")
    set(_backend SYSMPU)
  else()
    message(FATAL_ERROR "KickOS: the region unit `${unit}` has no Arm MPU backend")
  endif()
  set(${out_backend} "${_backend}" PARENT_SCOPE)
  set(${out_source} "${_source}" PARENT_SCOPE)
endfunction()

# kickos_chip_protection_disagreement(<out> <have_mpu> <have_aspace> <region_unit> <translates>
#                                     <chip_file>)
#   Empty when the configuration's enforcing posture and translating backend agree with the chip
#   file's protection unit, else why they do not.
function(kickos_chip_protection_disagreement out have_mpu have_aspace region_unit translates chip_file)
  set(_said "")
  if(have_mpu AND region_unit STREQUAL "")
    set(_said "the configuration resolved the enforcing region posture, and ${chip_file} states no "
              "region unit a build drives, so enforcement would be a silent no-op. Either drop the "
              "chip's `select HAS_MPU`, or carve the port's window for its unit and drop "
              "`driven: false`.")
  elseif(have_aspace AND NOT translates)
    set(_said "arch/Kconfig selects HAS_ASPACE for this chip, and ${chip_file} states no `mmu`, so "
              "the frame allocator would run over a pool no script carved.")
  elseif(translates AND NOT have_aspace)
    set(_said "${chip_file} states `mmu`, and arch/Kconfig does not select HAS_ASPACE for this chip.")
  elseif(translates AND have_mpu)
    set(_said "the configuration resolved both the enforcing region posture and translation, which "
              "no backend implements together.")
  endif()
  string(JOIN "" _said ${_said})
  set(${out} "${_said}" PARENT_SCOPE)
endfunction()
