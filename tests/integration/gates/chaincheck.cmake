# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The whole-chain gate riding `chaincheck`, one image per half, on every emulator and the sim. A
# board with no emulator registers no test here, and the script judges the board's capture. Where
# the integer half is cut in two images, `chaincheck` judges the core half and `chaincheck_print`
# the print half.

if(NOT TARGET chaincheck)
  return()
endif()

set(_chaincheck_judge "${PROJECT_SOURCE_DIR}/tests/integration/check_chaincheck.sh")
set(_chaincheck_int int)
if(TARGET chaincheck_print)
  set(_chaincheck_int core)
endif()
foreach(_chaincheck_half IN ITEMS int print float full)
  set(_chaincheck_image chaincheck_${_chaincheck_half})
  if(_chaincheck_half STREQUAL "int")
    set(_chaincheck_image chaincheck)
    set(_chaincheck_half ${_chaincheck_int})
  endif()
  if(NOT TARGET ${_chaincheck_image})
    continue()
  endif()
  kickos_app_judge(${_chaincheck_image} tests/integration/check_chaincheck.sh
                   ARGS ${_chaincheck_half})
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME ${_chaincheck_image}
      COMMAND "${_chaincheck_judge}" "$<TARGET_FILE:${_chaincheck_image}>" ${_chaincheck_half})
    kickos_boot_timeout(${_chaincheck_image} "${_chaincheck_judge}")
  else()
    kickos_add_qemu_test(TARGET ${_chaincheck_image} SCRIPT "${_chaincheck_judge}"
                         ARGS ${_chaincheck_half})
  endif()
endforeach()
