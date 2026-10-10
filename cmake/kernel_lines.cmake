# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_kernel_lines(<out> <chip> <rings> <lines> <doorbell_lines>)
#   The lines a build gives its kernel, from the generator's `SYMBOL=line` lists in chip.cmake:
#   every one of <lines> (KICKOS_CHIP_KERNEL_LINES), and <doorbell_lines>
#   (KICKOS_CHIP_DOORBELL_KERNEL_LINES) only where <rings> is true. Sets <out> to the line
#   numbers. An entry that is not `SYMBOL=line` stops the configure, naming <chip>.
function(kickos_kernel_lines out chip rings lines doorbell_lines)
  set(_entries ${lines})
  if(rings)
    list(APPEND _entries ${doorbell_lines})
  endif()
  set(_numbers "")
  foreach(_entry IN LISTS lines doorbell_lines)
    if(NOT _entry MATCHES "^[A-Za-z_][A-Za-z0-9_]*=[0-9]+$")
      message(FATAL_ERROR "KickOS: chip `${chip}`'s kernel line `${_entry}` is not SYMBOL=line, "
        "so the kernel would leave a line it drives claimable")
    endif()
  endforeach()
  foreach(_entry IN LISTS _entries)
    string(REGEX REPLACE "^.*=" "" _line "${_entry}")
    list(APPEND _numbers "${_line}")
  endforeach()
  set(${out} "${_numbers}" PARENT_SCOPE)
endfunction()
