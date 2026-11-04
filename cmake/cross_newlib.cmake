# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The newlib an image links: the KickOS toolchain's own, in the compiler's sysroot.

# _kickos_multilib_file(<out> <cc> <file> <flags>...)
#
# Sets <out> to the path of <file> in the library directory of the multilib these flags select, or
# to "" where that directory lacks it: -print-file-name falls back to the default multilib's.
function(_kickos_multilib_file _out _cc _file)
  set(${_out} "" PARENT_SCOPE)
  execute_process(COMMAND "${_cc}" ${ARGN} -print-multi-directory
                  OUTPUT_VARIABLE _multi OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND "${_cc}" ${ARGN} -print-file-name=${_file}
                  OUTPUT_VARIABLE _path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT IS_ABSOLUTE "${_path}" OR NOT EXISTS "${_path}")
    return()
  endif()
  if(NOT _multi STREQUAL ".")
    set(_tail "/${_multi}/${_file}")
    string(LENGTH "${_path}" _n)
    string(LENGTH "${_tail}" _t)
    if(_n LESS _t)
      return()
    endif()
    math(EXPR _at "${_n} - ${_t}")
    string(SUBSTRING "${_path}" ${_at} -1 _end)
    if(NOT _end STREQUAL "${_tail}")
      return()
    endif()
  endif()
  set(${_out} "${_path}" PARENT_SCOPE)
endfunction()

# kickos_require_toolchain_newlib(<label> <cc> <flags>...)
#
# The newlib profiles the KickOS toolchain carries for these flags (docs/design-m10-toolchain.md):
# full always, nano where the multilib's own directory holds libc_nano.a, which must come with
# nano.specs and the profile's other archives. Each must reach its state through __getreent, and
# nano must answer with its own header. Sets KICKOS_LIBC_PROFILES, the profiles found, and
# KICKOS_LIBC_DEFAULT, the board descriptor's KICKOS_BOARD_NEWLIB or full.
function(kickos_require_toolchain_newlib _label _cc)
  set(_flags ${ARGN})
  set(_default full)
  if(DEFINED KICKOS_BOARD_NEWLIB)
    set(_default "${KICKOS_BOARD_NEWLIB}")
  endif()
  set(_stamp "${_cc}|${_flags}")
  if("$CACHE{KICKOS_TOOLCHAIN_NEWLIB_OK_${_label}}" STREQUAL "${_stamp}")
    set(_profiles "$CACHE{KICKOS_LIBC_PROFILES}")
  else()
    set(_profiles "")
    foreach(_profile full nano)
      set(_libc libc.a)
      set(_specs "")
      set(_nano_check "#if defined(_NANO_FORMATTED_IO)\n#error KICKOS_PROBE_NANO\n#endif\n")
      if(_profile STREQUAL "nano")
        set(_libc libc_nano.a)
        set(_specs "--specs=nano.specs")
        set(_nano_check "#if !defined(_NANO_FORMATTED_IO)\n#error KICKOS_PROBE_FULL\n#endif\n")
      endif()
      if(_profile STREQUAL "full")
        execute_process(COMMAND "${_cc}" ${_flags} -print-file-name=${_libc}
                        OUTPUT_VARIABLE _path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT IS_ABSOLUTE "${_path}" OR NOT EXISTS "${_path}")
          message(FATAL_ERROR "KickOS ${_label} toolchain: ${_cc} carries no ${_libc} for "
            "'${_flags}', so the KickOS toolchain was built without this board's multilib")
        endif()
      else()
        _kickos_multilib_file(_path "${_cc}" ${_libc} ${_flags})
        if(_path STREQUAL "")
          continue()
        endif()
        set(_missing "")
        foreach(_file nano.specs libg_nano.a libstdc++_nano.a libsupc++_nano.a)
          _kickos_multilib_file(_found "${_cc}" ${_file} ${_flags})
          if(_found STREQUAL "")
            list(APPEND _missing ${_file})
          endif()
        endforeach()
        if(NOT _missing STREQUAL "")
          string(REPLACE ";" ", " _missing "${_missing}")
          message(FATAL_ERROR "KickOS ${_label} toolchain: ${_cc} carries ${_path} for "
            "'${_flags}' but no ${_missing} beside it, so its nano newlib is incomplete")
        endif()
      endif()
      get_filename_component(_bin "${_cc}" DIRECTORY)
      get_filename_component(_cc_name "${_cc}" NAME)
      string(REGEX REPLACE "gcc$" "nm" _nm_name "${_cc_name}")
      execute_process(COMMAND "${_bin}/${_nm_name}" -A "${_path}"
                      OUTPUT_VARIABLE _nm ERROR_QUIET RESULT_VARIABLE _rc)
      string(REGEX MATCHALL " U ${KICKOS_C_SYMBOL_PREFIX}__getreent\n" _callers "${_nm}")
      string(REGEX MATCHALL " [TtWw] ${KICKOS_C_SYMBOL_PREFIX}__getreent\n" _definers "${_nm}")
      list(LENGTH _callers _ncallers)
      list(LENGTH _definers _ndefiners)
      set(_ok TRUE)
      if(NOT _rc EQUAL 0 OR _ncallers EQUAL 0 OR NOT _ndefiners EQUAL 0)
        set(_ok FALSE)
      endif()
      set(_probe "${CMAKE_BINARY_DIR}/CMakeFiles/kickos-toolchain-newlib-probe-${_label}.c")
      set(_reent_check "#if !defined(__DYNAMIC_REENT__)\n#error KICKOS_PROBE_STATIC_REENT\n#endif\n")
      file(WRITE "${_probe}" "#include <sys/reent.h>\n${_reent_check}${_nano_check}int kickos_probe;\n")
      execute_process(COMMAND "${_cc}" ${_flags} ${_specs} -fsyntax-only "${_probe}"
                      RESULT_VARIABLE _hrc OUTPUT_QUIET ERROR_VARIABLE _herr)
      if(NOT _ok OR NOT _hrc EQUAL 0)
        string(STRIP "${_herr}" _herr)
        message(FATAL_ERROR "KickOS ${_label} toolchain: the ${_profile} newlib in ${_path} is "
          "not dynamic-reent: ${_ncallers} member(s) call __getreent, ${_ndefiners} define it, "
          "and the header probe answered '${_herr}'")
      endif()
      list(APPEND _profiles ${_profile})
      message(STATUS "KickOS ${_label} toolchain: ${_profile} dynamic-reent newlib from ${_path}")
    endforeach()
    set(KICKOS_LIBC_PROFILES "${_profiles}" CACHE INTERNAL "The newlib profiles the toolchain carries")
    set(KICKOS_TOOLCHAIN_NEWLIB_OK_${_label} "${_stamp}" CACHE INTERNAL "")
  endif()
  if(NOT _default IN_LIST _profiles)
    message(FATAL_ERROR "KickOS ${_label} toolchain: board '${KICKOS_BOARD}' makes newlib "
      "profile '${_default}' its images' default, and this toolchain carries only "
      "'${_profiles}' for its flags")
  endif()
  set(KICKOS_LIBC_DEFAULT "${_default}" CACHE INTERNAL "The newlib profile of an image that names none")
endfunction()

# kickos_reent_state(<full|nano> <out>)
#
# After project(): sets <out> to sizeof(struct _reent) in that profile's newlib, read off a
# compiled object's symbol size under the C flags every image compiles with.
function(kickos_reent_state _profile _out)
  set(_specs "")
  if(_profile STREQUAL "nano")
    set(_specs "--specs=nano.specs")
  endif()
  separate_arguments(_cflags UNIX_COMMAND "${CMAKE_C_FLAGS}")
  set(_stamp "${CMAKE_C_COMPILER}|${_cflags}|${_profile}")
  if("$CACHE{KICKOS_REENT_STATE_STAMP_${_profile}}" STREQUAL "${_stamp}")
    set(${_out} "$CACHE{KICKOS_REENT_STATE_OF_${_profile}}" PARENT_SCOPE)
    return()
  endif()
  set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/kickos-reent-${_profile}")
  file(MAKE_DIRECTORY "${_dir}")
  file(WRITE "${_dir}/probe.c"
    "#include <sys/reent.h>\nchar kickos_reent_probe[sizeof(struct _reent)] = {1};\n")
  execute_process(COMMAND "${CMAKE_C_COMPILER}" ${_cflags} ${_specs} -c "${_dir}/probe.c"
                          -o "${_dir}/probe.o"
                  RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "KickOS: cannot measure the ${_profile} newlib's struct _reent:\n${_err}")
  endif()
  execute_process(COMMAND "${CMAKE_NM}" -S "${_dir}/probe.o" OUTPUT_VARIABLE _nm
                  RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0
     OR NOT _nm MATCHES "[0-9a-fA-F]+ ([0-9a-fA-F]+) [DdBb] ${KICKOS_C_SYMBOL_PREFIX}kickos_reent_probe")
    message(FATAL_ERROR "KickOS: ${CMAKE_NM} gave no size for the ${_profile} struct _reent "
      "probe:\n${_nm}")
  endif()
  math(EXPR _size "0x${CMAKE_MATCH_1}" OUTPUT_FORMAT DECIMAL)
  set(KICKOS_REENT_STATE_OF_${_profile} "${_size}" CACHE INTERNAL "")
  set(KICKOS_REENT_STATE_STAMP_${_profile} "${_stamp}" CACHE INTERNAL "")
  set(${_out} "${_size}" PARENT_SCOPE)
endfunction()
