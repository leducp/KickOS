# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Feeds the chip linker script the numbers it needs to reject, AT LINK TIME, an arena
# too small for kmain's two boot stacks (arch/common/boot_arena.ld.h holds the ASSERT).
#
# Every input is READ BACK from the file that already owns it -- the sizes from the
# board's board_config.h through the real preprocessor, the MPU granule out of the
# arch_mpu_min_region() definition that the link will resolve -- so this file holds no
# second copy of any provisioning number and cannot drift into modelling a geometry the
# allocator does not produce. Anything it fails to read is a FATAL_ERROR, never a
# guessed default: a wrong-but-quiet model would put the boot panic back.

# Round `want` up to the region size one MPU descriptor can name. MIRRORS
# arch_ram_region_size() (arch/include/kickos/arch/arch.h) -- keep the two in step.
function(kickos_region_size want mn out)
  if(mn EQUAL 0)
    math(EXPR _v "((${want}) + 15) & ~15") # no MPU: 16-byte granular, no pow2 shaping
    set(${out} "${_v}" PARENT_SCOPE)
    return()
  endif()
  set(_w "${want}")
  if(_w LESS "${mn}")
    set(_w "${mn}")
  endif()
  set(_p 1)
  while(_p LESS "${_w}")
    math(EXPR _p "${_p} * 2")
  endwhile()
  set(${out} "${_p}" PARENT_SCOPE)
endfunction()

# Natural alignment the block must sit on. MIRRORS arch_ram_region_align().
function(kickos_region_align want mn out)
  if(mn EQUAL 0)
    set(${out} 16 PARENT_SCOPE)
    return()
  endif()
  kickos_region_size("${want}" "${mn}" _v)
  set(${out} "${_v}" PARENT_SCOPE)
endfunction()

# The integer returned by an arch_mpu_min_region() DEFINITION in `file`, or "" when the
# file only declares/calls it. Body-scoped so a mention in a comment cannot match.
function(_kickos_min_region_in_file file out)
  # Read at CONFIGURE time, so edits to the definition must re-run the configure that
  # baked its value into the linker script.
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
               APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
  file(READ "${file}" _txt)
  if("${_txt}" MATCHES "arch_mpu_min_region\\(void\\)[ \t\r\n]*{([^}]*)}")
    set(_body "${CMAKE_MATCH_1}")
    if("${_body}" MATCHES "return[ \t\r\n]+([0-9]+)")
      set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
      return()
    endif()
    message(FATAL_ERROR
      "KickOS: arch_mpu_min_region() in ${file} does not return a plain integer, so the "
      "link-time boot-stack model cannot read the MPU granule. Return a literal there, "
      "or teach cmake/boot_arena.cmake how to read it.")
  endif()
  set(${out} "" PARENT_SCOPE)
endfunction()

# The granule this link will actually resolve to: the chip's strong definition when it
# has one, else the arch family's weak default -- the same precedence as the link.
function(kickos_mpu_min_region arch_dir family chip out)
  file(GLOB_RECURSE _arch_srcs "${arch_dir}/${family}/*.cc")
  list(FILTER _arch_srcs EXCLUDE REGEX "/chip/")
  set(_weak "")
  foreach(_f IN LISTS _arch_srcs)
    _kickos_min_region_in_file("${_f}" _v)
    if(NOT _v STREQUAL "")
      if(NOT _weak STREQUAL "" AND NOT _weak STREQUAL "${_v}")
        message(FATAL_ERROR
          "KickOS: two arch backends under ${arch_dir}/${family} define "
          "arch_mpu_min_region() with different values (${_weak} and ${_v}); the "
          "link-time boot-stack model cannot tell which one this link resolves to.")
      endif()
      set(_weak "${_v}")
    endif()
  endforeach()
  set(_strong "")
  file(GLOB _chip_srcs "${arch_dir}/${family}/chip/${chip}/*.cc")
  foreach(_f IN LISTS _chip_srcs)
    _kickos_min_region_in_file("${_f}" _v)
    if(NOT _v STREQUAL "")
      set(_strong "${_v}")
    endif()
  endforeach()
  if(NOT _strong STREQUAL "")
    set(${out} "${_strong}" PARENT_SCOPE)
    return()
  endif()
  if(_weak STREQUAL "")
    message(FATAL_ERROR
      "KickOS: no arch_mpu_min_region() definition found for family '${family}' chip "
      "'${chip}', so the link-time boot-stack model has no MPU granule.")
  endif()
  set(${out} "${_weak}" PARENT_SCOPE)
endfunction()

# The two boot-stack sizes AS THE COMPILE WILL SEE THEM: the effective board_config.h
# run through the real preprocessor with the same -D overrides, so a command-line
# override can never leave the link-time model describing the header's value instead.
function(kickos_boot_stack_sizes board_inc overrides out_idle out_root)
  # Same reason as above: a size edited in the header must re-run the configure.
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${board_inc}/kickos/board_config.h")
  set(_probe "${CMAKE_CURRENT_BINARY_DIR}/boot_stack_probe.c")
  file(WRITE "${_probe}"
    "#include <kickos/board_config.h>\n"
    "#ifndef KICKOS_IDLE_STACK_SIZE\n"
    "#error \"KICKOS_IDLE_STACK_SIZE unset\"\n"
    "#endif\n"
    "#ifndef KICKOS_ROOT_STACK_SIZE\n"
    "#error \"KICKOS_ROOT_STACK_SIZE unset\"\n"
    "#endif\n"
    "kickos_boot_idle KICKOS_IDLE_STACK_SIZE\n"
    "kickos_boot_root KICKOS_ROOT_STACK_SIZE\n")
  set(_flags "")
  foreach(_o IN LISTS overrides)
    list(APPEND _flags "-D${_o}")
  endforeach()
  execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -E -P -x c "-I${board_inc}" ${_flags} "${_probe}"
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
      "KickOS: could not read the boot-stack sizes from ${board_inc}/kickos/board_config.h. "
      "Every board must state KICKOS_IDLE_STACK_SIZE and KICKOS_ROOT_STACK_SIZE (the "
      "64 KiB system.h defaults do not fit an MCU arena).\n${_err}")
  endif()
  foreach(_k idle root)
    if(NOT "${_out}" MATCHES "kickos_boot_${_k}[ \t]+([^\r\n]+)")
      message(FATAL_ERROR "KickOS: boot-stack probe produced no ${_k} size:\n${_out}")
    endif()
    math(EXPR _v "${CMAKE_MATCH_1}")
    set(_size_${_k} "${_v}")
  endforeach()
  set(${out_idle} "${_size_idle}" PARENT_SCOPE)
  set(${out_root} "${_size_root}" PARENT_SCOPE)
endfunction()

# The -D set arch/common/boot_arena.ld.h expects. Also refuses a chip linker script
# that carries no KICKOS_BOOT_ARENA_ASSERT, so no board can opt out by omission.
function(kickos_boot_arena_defs arch_dir family chip board_inc ld overrides out)
  kickos_mpu_min_region("${arch_dir}" "${family}" "${chip}" _mn)
  kickos_boot_stack_sizes("${board_inc}" "${overrides}" _idle _root)
  kickos_region_size("${_idle}" "${_mn}" _isz)
  kickos_region_align("${_idle}" "${_mn}" _ial)
  kickos_region_size("${_root}" "${_mn}" _rsz)
  kickos_region_align("${_root}" "${_mn}" _ral)
  file(READ "${ld}" _ldtxt)
  if(NOT "${_ldtxt}" MATCHES "KICKOS_BOOT_ARENA_ASSERT")
    message(FATAL_ERROR
      "KickOS: ${ld} does not invoke KICKOS_BOOT_ARENA_ASSERT, so this board would ship "
      "an arena too small for the boot stacks as a runtime kpanic instead of a link "
      "error. Include <boot_arena.ld.h> and invoke it beside the existing "
      "__kickos_ram_start <= __kickos_ram_end assert.")
  endif()
  message(STATUS "KickOS: boot stacks idle=${_idle}->${_isz}/${_ial} "
                 "root=${_root}->${_rsz}/${_ral} (mpu granule ${_mn})")
  set(${out}
    "-DKICKOS_BOOT_IDLE_SIZE=${_isz}" "-DKICKOS_BOOT_IDLE_ALIGN=${_ial}"
    "-DKICKOS_BOOT_ROOT_SIZE=${_rsz}" "-DKICKOS_BOOT_ROOT_ALIGN=${_ral}"
    PARENT_SCOPE)
endfunction()
