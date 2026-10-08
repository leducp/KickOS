# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Feeds the chip linker script the numbers it needs to reject, AT LINK TIME, an arena
# too small for kmain's two boot stacks (arch/common/boot_arena.ld.h holds the ASSERT).
#
# Every input is scraped from the file that already owns it, and anything unreadable is
# a FATAL_ERROR, never a guessed default.

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
  find_program(KICKOS_UV uv)
  if(NOT KICKOS_UV)
    message(FATAL_ERROR "KickOS: uv not found on PATH; the RAM region rule is answered by "
      "tools/compose, which runs under uv (https://docs.astral.sh/uv/)")
  endif()
  # From the resolved configuration, as the C side reads it: a caller-passed stride could model a
  # geometry the allocator does not produce.
  set(_stride 0)
  if(KICKOS_TLS AND KICKOS_TLS_FROM_SP)
    set(_stride "${KICKOS_STACK_STRIDE}")
  endif()
  math(EXPR _bits "${CMAKE_SIZEOF_VOID_P} * 8")
  set(_tool "${PROJECT_SOURCE_DIR}/tools/compose")
  # The whole package: `python -m kickos_compose` imports every module of it.
  file(GLOB _tool_sources "${_tool}/kickos_compose/*.py")
  set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${_tool_sources} "${_tool}/pyproject.toml" "${_tool}/uv.lock")
  # Run from the tool's directory, which `-m` puts first on sys.path, never the build's.
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "UV_PROJECT_ENVIRONMENT=${CMAKE_BINARY_DIR}/kickos_compose/venv"
            UV_PYTHON_DOWNLOADS=never PYTHONDONTWRITEBYTECODE=1
            "${KICKOS_UV}" run --project "${_tool}" --locked --quiet
            python -m kickos_compose region --min-region "${mn}" --pow2 "${pow2}"
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

# The integer returned by a `symbol` DEFINITION in `file`, or "" when the file only
# declares it. The body match stops at the first `}`, so no comment inside a scraped
# body may carry a closing brace.
function(_kickos_seam_int_in_file file symbol out)
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
               APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
  file(READ "${file}" _txt)
  # The leading non-identifier boundary is required: a bare suffix match would read a
  # longer identifier ending in `symbol` as the definition of `symbol`.
  if("${_txt}" MATCHES "[^A-Za-z0-9_]${symbol}\\(void\\)[ \t\r\n]*{([^}]*)}")
    set(_body "${CMAKE_MATCH_1}")
    # The trailing `;` is required: without it `return 32 + 0;` reads as 32.
    if("${_body}" MATCHES "return[ \t\r\n]+([0-9]+)[uU]?[ \t\r\n]*;")
      set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
      return()
    endif()
    message(FATAL_ERROR
      "KickOS: ${symbol}() in ${file} does not return a plain integer, so the "
      "link-time boot-stack model cannot read it. Return a literal there, "
      "or teach cmake/boot_arena.cmake how to read it.")
  endif()
  set(${out} "" PARENT_SCOPE)
endfunction()

# The .cc sources of `tgt`, absolute. The arch libraries were added with paths relative
# to `basedir` and the chip library with absolute ones, so both shapes must be accepted.
function(_kickos_target_cc_sources tgt basedir out)
  get_target_property(_srcs ${tgt} SOURCES)
  set(_abs "")
  foreach(_s IN LISTS _srcs)
    if(NOT "${_s}" MATCHES "\\.cc$")
      continue()
    endif()
    if(IS_ABSOLUTE "${_s}")
      list(APPEND _abs "${_s}")
    else()
      list(APPEND _abs "${basedir}/${_s}")
    endif()
  endforeach()
  set(${out} "${_abs}" PARENT_SCOPE)
endfunction()

# The integer this link resolves `symbol` to. There is no arch-vs-chip precedence to
# model: every seam is defined ONCE by a backend, or not at all and then once by its
# <symbol>_default.cc fallback TU (arch/CMakeLists.txt states the rule). So the model is
# "the single backend definition, else the fallback", and two backend definitions are a
# build bug rather than something to resolve by scan order.
function(_kickos_seam_int arch_srcs chip_srcs symbol out)
  set(_backend "")
  set(_backend_file "")
  set(_fallback "")
  foreach(_f IN LISTS chip_srcs arch_srcs)
    _kickos_seam_int_in_file("${_f}" "${symbol}" _v)
    if(_v STREQUAL "")
      continue()
    endif()
    get_filename_component(_base "${_f}" NAME)
    if(_base MATCHES "_default\\.cc$")
      if(NOT _fallback STREQUAL "")
        message(FATAL_ERROR
          "KickOS: ${symbol}() has more than one fallback TU; exactly one "
          "<symbol>_default.cc may define it.")
      endif()
      set(_fallback "${_v}")
      continue()
    endif()
    if(NOT _backend STREQUAL "")
      message(FATAL_ERROR
        "KickOS: ${_backend_file} and ${_base} both define ${symbol}(), so this link "
        "has two backend definitions and would fail on whichever archive member the "
        "linker extracts first. Exactly one backend may define a seam.")
    endif()
    set(_backend "${_v}")
    set(_backend_file "${_base}")
  endforeach()
  if(NOT _backend STREQUAL "")
    set(${out} "${_backend}" PARENT_SCOPE)
    return()
  endif()
  set(${out} "${_fallback}" PARENT_SCOPE)
endfunction()

# The -D set arch/common/boot_arena.ld.h expects. Also refuses a chip linker script
# that carries neither arena assert, so no board can opt out by omission.
# out_mn and out_pow2 hand back the raw scraped seam integers; every other output is
# derived from them.
function(kickos_boot_arena_defs arch_dir arch_tgt chip_tgt ld
                                out out_mn out_pow2)
  _kickos_target_cc_sources("${arch_tgt}" "${arch_dir}" _arch_srcs)
  _kickos_target_cc_sources("${chip_tgt}" "${arch_dir}" _chip_srcs)
  _kickos_seam_int("${_arch_srcs}" "${_chip_srcs}" "arch_mpu_min_region" _mn)
  _kickos_seam_int("${_arch_srcs}" "${_chip_srcs}" "arch_mpu_region_pow2" _p2)
  foreach(_sym mn p2)
    if(_${_sym} STREQUAL "")
      message(FATAL_ERROR
        "KickOS: neither ${arch_tgt} nor ${chip_tgt} defines the region-encoding seam "
        "(arch_mpu_min_region / arch_mpu_region_pow2), so the link-time boot-stack model "
        "has no region geometry. Every arch backend must define both, as a backend TU or "
        "as a <symbol>_default.cc fallback.")
    endif()
  endforeach()
  # From the generated fragment, the resolution the compile reads: any other copy of these
  # numbers models an image nobody builds. KICKOS_MAX_THREADS is the slots but root's, whose stack
  # is the boot replay's.
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
  # what each thread costs: refused, never modelled. _mn first: a no-MPU board scrapes _p2 = 1
  # off the v7-M fallback TU and snaps nothing.
  if(_p2 AND NOT _mn EQUAL 0 AND NOT _user EQUAL 0)
    math(EXPR _user_pow2 "${_user} & (${_user} - 1)")
    if(NOT _user_pow2 EQUAL 0)
      message(FATAL_ERROR
        "KickOS: KICKOS_USER_STACK_SIZE is ${_user} on board '${KICKOS_BOARD}', whose MPU "
        "backend encodes a region as a power of two (arch_mpu_region_pow2 returns 1). "
        "arch_ram_region_size would snap every demand-allocated stack up to the next power "
        "of two, so each thread would silently cost more than the defconfig states. State a "
        "power of two, or state a size this backend can name exactly.")
    endif()
  endif()
  kickos_ram_region_rule("${_mn}" "${_p2}" _figures "${_idle}" "${_root}" "${_user}")
  list(GET _figures 0 _isz)
  list(GET _figures 1 _ial)
  list(GET _figures 2 _rsz)
  list(GET _figures 3 _ral)
  list(GET _figures 4 _usz)
  list(GET _figures 5 _ual)
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
  # pow2 is posture-dependent on a v8-M chip: arch_arm_pmsav8.cc (the pow2=0 backend)
  # enters the link only at KICKOS_HAVE_MPU=1, so qemu-m33 and pizero2350 scrape pow2=1
  # from the v7-M fallback TU without it. frdmk64f and rx72m compile theirs always.
  message(STATUS "KickOS: boot stacks idle=${_idle}->${_isz}/${_ial} "
                 "root=${_root}->${_rsz}/${_ral} (mpu granule ${_mn} pow2=${_p2})")
  # Machine-readable for the fleet headroom sweep: only the linker knows the arena base,
  # so the exact fit (alignment run-ups included) is asserted in boot_arena.ld.h. These
  # are the demand terms that assert replays.
  message(STATUS "KickOS: arena model idle=${_isz}/${_ial} root=${_rsz}/${_ral} "
                 "pool=${_stacks}x${_usz}/${_ual}")
  set(${out}
    "-DKICKOS_BOOT_IDLE_SIZE=${_isz}" "-DKICKOS_BOOT_IDLE_ALIGN=${_ial}"
    "-DKICKOS_BOOT_ROOT_SIZE=${_rsz}" "-DKICKOS_BOOT_ROOT_ALIGN=${_ral}"
    "-DKICKOS_POOL_STACK_SIZE=${_usz}" "-DKICKOS_POOL_STACK_ALIGN=${_ual}"
    "-DKICKOS_POOL_STACK_COUNT=${_stacks}"
    PARENT_SCOPE)
  set(${out_mn} "${_mn}" PARENT_SCOPE)
  set(${out_pow2} "${_p2}" PARENT_SCOPE)
endfunction()
