# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Emits one TAB-separated line per board that needs an emulator:
#
#   <board> <TAB> <emulator binary>
#
# Run as: cmake -DSRC=<repo root> -DBOARDS=<;-list> -DOUT=<file> -P tests/static/board_emulators.cmake
#
# The answer comes from kickos_qemu_machine, the one body that maps a board to its emulator,
# so a sweep cannot disagree with what a ctest registration would spend. A board that boots on
# silicon emits no line.

if(NOT DEFINED SRC OR NOT DEFINED OUT OR NOT DEFINED BOARDS)
  message(FATAL_ERROR "board_emulators.cmake needs -DSRC=, -DBOARDS= and -DOUT=")
endif()

include("${SRC}/cmake/kickos.cmake")

set(_lines "")
foreach(_board IN LISTS BOARDS)
  kickos_qemu_machine("${_board}" _env _machine)
  set(_emu "")
  foreach(_kv IN LISTS _env)
    if(_kv MATCHES "^QEMU=(.+)$")
      set(_emu "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  # A machine with no QEMU= in its env is an arm one: qemu-system-arm is the default the
  # gate runner falls back to, and only the boards needing another binary name it.
  if(_emu STREQUAL "" AND NOT _machine STREQUAL "")
    set(_emu qemu-system-arm)
  endif()
  if(NOT _emu STREQUAL "")
    string(APPEND _lines "${_board}\t${_emu}\n")
  endif()
endforeach()

if(_lines STREQUAL "")
  message(FATAL_ERROR "no board resolved to an emulator; the map would claim the fleet boots natively")
endif()

file(WRITE "${OUT}" "${_lines}")
