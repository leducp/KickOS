# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Feeds the chip linker script the figures arch/common/boot_arena.ld.h asserts the arena
# against. Each input is read from the file that owns it; an unreadable one is a FATAL_ERROR.

function(kickos_pow2_ceil want out)
  set(_p 1)
  while(_p LESS want)
    math(EXPR _p "${_p} * 2")
  endwhile()
  set(${out} "${_p}" PARENT_SCOPE)
endfunction()

# The RAM region rule as the compose tool answers it on this build (kickos_compose/region.py): the
# no-unit granule into the KICKOS_NO_UNIT_GRANULE property, and into `out` the size, then the
# alignment, of each further argument, as arch_ram_region_size and arch_ram_region_align give them.
# A block the target's address space cannot hold fails the configure.
function(kickos_ram_region_rule mn pow2 out)
  # From the resolved configuration, as the C side reads it: a caller-passed stride could model a
  # geometry the allocator does not produce.
  set(_stride 0)
  if(KICKOS_TLS AND KICKOS_TLS_FROM_SP)
    set(_stride "${KICKOS_STACK_STRIDE}")
  endif()
  math(EXPR _bits "${CMAKE_SIZEOF_VOID_P} * 8")
  get_property(_tool GLOBAL PROPERTY KICKOS_COMPOSE_TOOL)
  kickos_compose_inputs(_inputs "${_tool}")
  set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})
  set(_tmp "${CMAKE_BINARY_DIR}/kickos_compose/region/tmp")
  file(MAKE_DIRECTORY "${_tmp}")
  kickos_compose_python(_python "${_tool}" "${CMAKE_BINARY_DIR}/kickos_compose/venv" "${_tmp}")
  # Run from the tool's directory, which `-m` puts first on sys.path, never the build's.
  execute_process(
    COMMAND ${_python} -m kickos_compose region --min-region "${mn}" --pow2 "${pow2}"
            --stride "${_stride}" --word-bits "${_bits}" ${ARGN}
    WORKING_DIRECTORY "${_tool}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "KickOS: the compose tool refused the RAM region rule on board "
      "'${KICKOS_BOARD}' (${_rc}):\n  ${_out}${_err}")
  endif()
  string(REGEX MATCHALL "[0-9]+" _figures "${_out}")
  list(POP_FRONT _figures _granule)
  set_property(GLOBAL PROPERTY KICKOS_NO_UNIT_GRANULE "${_granule}")
  set(${out} "${_figures}" PARENT_SCOPE)
endfunction()

# The -D set arch/common/boot_arena.ld.h expects, and a refusal of a chip linker script that
# omits either arena assert.
function(kickos_boot_arena_defs ld out)
  set(_mn "${KICKOS_CHIP_REGION_GRANULE}")
  set(_p2 "${KICKOS_CHIP_REGION_POW2}")
  # From the generated fragment, the resolution the compile reads. KICKOS_MAX_THREADS is the slots
  # but root's, whose stack is the boot replay's.
  set(_idle "${KICKOS_IDLE_STACK_SIZE}")
  set(_root "${KICKOS_ROOT_STACK_SIZE}")
  set(_user "${KICKOS_USER_STACK_SIZE}")
  set(_stacks "${KICKOS_MAX_THREADS}")
  foreach(_v idle root user stacks)
    if(NOT "${_${_v}}" MATCHES "^[0-9]+$")
      message(FATAL_ERROR
        "KickOS: the boot-arena model has no ${_v} size. Every board states "
        "KICKOS_IDLE_STACK_SIZE and KICKOS_ROOT_STACK_SIZE in its defconfig, and the "
        "generated kickos_config.cmake is what carries them here.")
    endif()
  endforeach()
  # A pow2 backend snaps a stack that is not a power of two up, so the defconfig would not state
  # what each thread costs: refused, never modelled.
  if(_p2 AND NOT _user EQUAL 0)
    math(EXPR _user_pow2 "${_user} & (${_user} - 1)")
    if(NOT _user_pow2 EQUAL 0)
      message(FATAL_ERROR
        "KickOS: KICKOS_USER_STACK_SIZE is ${_user} on board '${KICKOS_BOARD}', whose "
        "protection unit names a region as a power of two (platform/${KICKOS_CHIP}/chip.yaml). "
        "arch_ram_region_size would snap every demand-allocated stack up to the next power "
        "of two, so each thread would silently cost more than the defconfig states. State a "
        "power of two, or state a size this backend can name exactly.")
    endif()
  endif()
  # The init's private block with one record, the least a system with a task reserves.
  kickos_ram_region_rule("${_mn}" "${_p2}" _figures "${_idle}" "${_root}"
                         "${KICKOS_INIT_PRIVATE_RECORD_SIZE}" "${_user}")
  list(GET _figures 0 _isz)
  list(GET _figures 1 _ial)
  list(GET _figures 2 _rsz)
  list(GET _figures 3 _ral)
  list(GET _figures 4 _psz)
  list(GET _figures 5 _pal)
  list(GET _figures 6 _usz)
  list(GET _figures 7 _ual)
  file(READ "${ld}" _ldtxt)
  if(NOT "${_ldtxt}" MATCHES "KICKOS_BOOT_ARENA_ASSERT")
    message(FATAL_ERROR
      "KickOS: ${ld} does not invoke KICKOS_BOOT_ARENA_ASSERT, so this board would ship "
      "an arena too small for the boot stacks as a runtime kpanic instead of a link "
      "error. Include <boot_arena.ld.h> and invoke it beside the existing "
      "__kickos_ram_start <= __kickos_ram_end assert.")
  endif()
  if(NOT "${_ldtxt}" MATCHES "KICKOS_POOL_ARENA_ASSERT")
    message(FATAL_ERROR
      "KickOS: ${ld} does not invoke KICKOS_POOL_ARENA_ASSERT, so this board would ship "
      "thread slots its arena cannot seat as a per-spawn -KOS_ENOMEM indistinguishable "
      "from a full slot table. Invoke it beside KICKOS_BOOT_ARENA_ASSERT with the same "
      "two arena symbols.")
  endif()
  message(STATUS "KickOS: boot stacks idle=${_idle}->${_isz}/${_ial} "
                 "root=${_root}->${_rsz}/${_ral} (mpu granule ${_mn} pow2=${_p2})")
  # Machine-readable for the fleet headroom sweep: the demand terms boot_arena.ld.h replays.
  message(STATUS "KickOS: arena model idle=${_isz}/${_ial} root=${_rsz}/${_ral} "
                 "private=${_psz}/${_pal} pool=${_stacks}x${_usz}/${_ual}")
  set(${out}
    "-DKICKOS_BOOT_IDLE_SIZE=${_isz}" "-DKICKOS_BOOT_IDLE_ALIGN=${_ial}"
    "-DKICKOS_BOOT_ROOT_SIZE=${_rsz}" "-DKICKOS_BOOT_ROOT_ALIGN=${_ral}"
    "-DKICKOS_BOOT_PRIVATE_SIZE=${_psz}" "-DKICKOS_BOOT_PRIVATE_ALIGN=${_pal}"
    "-DKICKOS_POOL_STACK_SIZE=${_usz}" "-DKICKOS_POOL_STACK_ALIGN=${_ual}"
    "-DKICKOS_POOL_STACK_COUNT=${_stacks}"
    PARENT_SCOPE)
endfunction()
