# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# One image per half, each carrying only its own arms: the integer half, the float half, and
# the float and C99 printf half. Newlib's nano profile prints no double, no long long and no C99
# length modifier, so the full-printf image links the full profile on every board. The float
# image overflows a 64 KiB flash, so a chip stating no more than that gets no float image, and
# its integer half is cut in two images: the arithmetic and memory arms, and the integer printf
# arms.
function(chaincheck_image name core print float full)
  add_executable(${name} main.c)
  target_link_libraries(${name} PRIVATE KickOS::kickos KickOS::system_default)
  target_compile_options(${name} PRIVATE -Wno-unused-function)
  set(_fp 0)
  if(float OR full)
    set(_fp 1)
  endif()
  target_compile_definitions(${name} PRIVATE CHAINCHECK_CORE=${core} CHAINCHECK_PRINT=${print}
                                             CHAINCHECK_FLOAT=${float}
                                             CHAINCHECK_FULL=${full} CHAINCHECK_FP=${_fp})
  kickos_emit_image(${name})
endfunction()

set(_chaincheck_small FALSE)
if(DEFINED KICKOS_CHIP_LINK_FLASH_LENGTH)
  math(EXPR _chaincheck_flash "${KICKOS_CHIP_LINK_FLASH_LENGTH}")
  if(_chaincheck_flash LESS_EQUAL 65536)
    set(_chaincheck_small TRUE)
  endif()
endif()
if(_chaincheck_small)
  chaincheck_image(chaincheck 1 0 0 0)
  chaincheck_image(chaincheck_print 0 1 0 0)
else()
  chaincheck_image(chaincheck 1 1 0 0)
  chaincheck_image(chaincheck_float 0 0 1 0)
endif()
chaincheck_image(chaincheck_full 0 0 0 1)
kickos_image_libc(chaincheck_full full)
