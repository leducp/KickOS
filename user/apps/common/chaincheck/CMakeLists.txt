# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# One image per half, each carrying only its own arms: the integer half, the float half, and
# the float and C99 printf half. Newlib's nano profile prints no double, no long long and no C99
# length modifier, so the full-printf image is not built there. The float image overflows a
# 64 KiB flash, so a chip stating no more than that gets the integer image alone.
function(chaincheck_image name int float full)
  add_executable(${name} main.c)
  target_link_libraries(${name} PRIVATE KickOS::kernel KickOS::system_default)
  target_compile_options(${name} PRIVATE -Wno-unused-function)
  set(_fp 0)
  if(float OR full)
    set(_fp 1)
  endif()
  target_compile_definitions(${name} PRIVATE CHAINCHECK_INT=${int} CHAINCHECK_FLOAT=${float}
                                             CHAINCHECK_FULL=${full} CHAINCHECK_FP=${_fp})
  kickos_emit_image(${name})
endfunction()

chaincheck_image(chaincheck 1 0 0)
set(_chaincheck_float_fits TRUE)
if(DEFINED KICKOS_CHIP_LINK_FLASH_LENGTH)
  math(EXPR _chaincheck_flash "${KICKOS_CHIP_LINK_FLASH_LENGTH}")
  if(_chaincheck_flash LESS_EQUAL 65536)
    set(_chaincheck_float_fits FALSE)
  endif()
endif()
if(_chaincheck_float_fits)
  chaincheck_image(chaincheck_float 0 1 0)
endif()
if(NOT KICKOS_NEWLIB_NANO)
  chaincheck_image(chaincheck_full 0 0 1)
endif()
