# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The processor a board's emulator line runs. tests/static/check_qemu_cpu.cmake includes this file
# in script mode, so it may use no project state.

# ---------------------------------------------------------------------------
# kickos_qemu_cpu_line(<out> <cpu> <board> [<option>...])
#   The board's emulator options with <cpu> in place of the processor their -cpu names; an
#   empty <cpu> keeps them. A line that names no -cpu is refused: its machine fixes the
#   processor, and an appended -cpu would not name the one the image runs on.
# ---------------------------------------------------------------------------
function(kickos_qemu_cpu_line out cpu board)
  set(_line ${ARGN})
  if(NOT "${cpu}" STREQUAL "")
    list(FIND _line "-cpu" _at)
    if(_at EQUAL -1)
      message(FATAL_ERROR "KickOS: QEMU_CPU_NO_SLOT: KICKOS_QEMU_CPU=${cpu} replaces the -cpu of "
        "board ${board}'s emulator line, which names none")
    endif()
    math(EXPR _at "${_at} + 1")
    list(REMOVE_AT _line ${_at})
    list(INSERT _line ${_at} "${cpu}")
  endif()
  set(${out} "${_line}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kickos_qemu_cpu_model(<out> <cpu> <mcpu-flags> [<option>...])
#   The processor model the emulator runs, without the properties a -cpu value carries after
#   commas: <cpu> where set, else the line's -cpu, else the core the chip's -mcpu names, which is
#   the one a machine that takes no -cpu models. Empty when none of them names one.
# ---------------------------------------------------------------------------
function(kickos_qemu_cpu_model out cpu mcpu)
  set(_line ${ARGN})
  set(_cpu "${cpu}")
  if(_cpu STREQUAL "")
    list(FIND _line "-cpu" _at)
    if(_at GREATER -1)
      math(EXPR _at "${_at} + 1")
      list(GET _line ${_at} _cpu)
    endif()
  endif()
  if(_cpu STREQUAL "" AND "${mcpu}" MATCHES "(^|[ ;])-mcpu=([A-Za-z0-9_.-]+)")
    set(_cpu "${CMAKE_MATCH_2}")
  endif()
  string(REGEX REPLACE ",.*$" "" _cpu "${_cpu}")
  set(${out} "${_cpu}" PARENT_SCOPE)
endfunction()
