# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Emits one TAB-separated line per board that needs an emulator:
#
#   <board> <TAB> <emulator binary>
#
# Run as: cmake -DSRC=<repo root> -DBOARDS=<;-list> -DOUT=<file> -P tests/static/board_emulators.cmake
#
# The answer is the `emulator` of each board's board file, read through the chip.cmake the chip
# generator writes for it, which is what a configure of that board hands kickos_qemu_machine. A
# board that boots on silicon emits no line.

if(NOT DEFINED SRC OR NOT DEFINED OUT OR NOT DEFINED BOARDS)
  message(FATAL_ERROR "board_emulators.cmake needs -DSRC=, -DBOARDS= and -DOUT=")
endif()
include("${SRC}/cmake/compose.cmake")
set(_scratch "${OUT}.d")
file(REMOVE_RECURSE "${_scratch}")

function(_emulator_of board out)
  set(_descriptor "${SRC}/boards/${board}/board.cmake")
  if(NOT EXISTS "${_descriptor}")
    message(FATAL_ERROR "board_emulators.cmake: '${board}' names no board (no ${_descriptor})")
  endif()
  include("${_descriptor}")
  set(_dir "${_scratch}/${board}")
  file(MAKE_DIRECTORY "${_dir}/tmp")
  kickos_compose_python(_python "${SRC}/tools/compose" "${_scratch}/venv" "${_dir}/tmp")
  execute_process(
    COMMAND ${_python} -m kickos_compose chip "${SRC}/platform/${KICKOS_CHIP}/${board}.yaml" --arch "${KICKOS_ARCH}"
            --include-dir "${_dir}/include" --chip-dir "${_dir}"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err OUTPUT_VARIABLE _out)
  if(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "board_emulators.cmake: the chip generator failed on board '${board}' (${_rc}):\n${_out}${_err}")
  endif()
  include("${_dir}/chip.cmake")
  set(${out} "${KICKOS_QEMU_BINARY}" PARENT_SCOPE)
endfunction()

set(_lines "")
foreach(_board IN LISTS BOARDS)
  _emulator_of("${_board}" _emu)
  if(NOT _emu STREQUAL "")
    string(APPEND _lines "${_board}\t${_emu}\n")
  endif()
endforeach()
file(REMOVE_RECURSE "${_scratch}")

if(_lines STREQUAL "")
  message(FATAL_ERROR "no board resolved to an emulator; the map would claim the fleet boots natively")
endif()

file(WRITE "${OUT}" "${_lines}")
