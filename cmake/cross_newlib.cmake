# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The newlib an image links: the KickOS toolchain's own, in the compiler's sysroot.

# kickos_require_toolchain_newlib(<label> <cc> <dynamic|static> [FLAVOR <full|nano>] <flags>...)
#
# The newlib the KickOS toolchain carries in its own tree (docs/design-m10-toolchain.md), which
# the compiler searches already: the libc these flags select is checked for the board's
# reentrancy, and nano for its header and archive. Sets _kos_newlib_c, _kos_newlib_cxx,
# _kos_newlib_link and _kos_newlib_refuse in the caller.
function(kickos_require_toolchain_newlib _label _cc _reent)
  if(NOT _reent MATCHES "^(dynamic|static)$")
    message(FATAL_ERROR "KickOS ${_label} toolchain: invalid newlib reentrancy '${_reent}'")
  endif()
  cmake_parse_arguments(PARSE_ARGV 3 _kos_tn "" "FLAVOR" "")
  set(_flags ${_kos_tn_UNPARSED_ARGUMENTS})
  set(_flavor full)
  set(_libc libc.a)
  set(_link "")
  set(_nano_check "#if defined(_NANO_FORMATTED_IO)\n#error KICKOS_PROBE_NANO\n#endif\n")
  if(_kos_tn_FLAVOR STREQUAL "nano")
    set(_flavor nano)
    set(_libc libc_nano.a)
    set(_link "--specs=nano.specs")
    set(_nano_check "#if !defined(_NANO_FORMATTED_IO)\n#error KICKOS_PROBE_FULL\n#endif\n")
  endif()
  set(KICKOS_NEWLIB_FLAVOR "${_flavor}" CACHE INTERNAL "Selected pinned newlib profile" FORCE)
  # In the compile flags, which CMake also hands the link driver: nano.specs is what puts the
  # nano newlib.h first as well as libc_nano.a, and KickOS's own per-thread newlib state is
  # sized by the struct _reent that header declares. Named twice, the driver refuses it.
  set(_kos_newlib_c "${_link}" PARENT_SCOPE)
  set(_kos_newlib_cxx "${_link}" PARENT_SCOPE)
  set(_kos_newlib_link "" PARENT_SCOPE)
  set(_kos_newlib_refuse "" PARENT_SCOPE)
  if(_flavor STREQUAL "full")
    set(_kos_newlib_refuse "--specs=nano.specs" PARENT_SCOPE)
  endif()
  set(_stamp "${_cc}|${_reent}|${_flavor}|${_flags}")
  if("$CACHE{KICKOS_TOOLCHAIN_NEWLIB_OK_${_label}}" STREQUAL "${_stamp}")
    return()
  endif()

  execute_process(COMMAND "${_cc}" ${_flags} -print-file-name=${_libc}
                  OUTPUT_VARIABLE _path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT IS_ABSOLUTE "${_path}" OR NOT EXISTS "${_path}")
    message(FATAL_ERROR "KickOS ${_label} toolchain: ${_cc} carries no ${_libc} for "
      "'${_flags}', so the KickOS toolchain was built without this board's multilib")
  endif()
  get_filename_component(_bin "${_cc}" DIRECTORY)
  get_filename_component(_cc_name "${_cc}" NAME)
  string(REGEX REPLACE "gcc$" "nm" _nm_name "${_cc_name}")
  execute_process(COMMAND "${_bin}/${_nm_name}" -A "${_path}"
                  OUTPUT_VARIABLE _nm ERROR_QUIET RESULT_VARIABLE _rc)
  string(REGEX MATCHALL " U __getreent\n" _callers "${_nm}")
  string(REGEX MATCHALL " [TtWw] __getreent\n" _definers "${_nm}")
  list(LENGTH _callers _ncallers)
  list(LENGTH _definers _ndefiners)
  set(_ok TRUE)
  if(NOT _rc EQUAL 0
     OR (_reent STREQUAL "dynamic" AND (_ncallers EQUAL 0 OR NOT _ndefiners EQUAL 0))
     OR (_reent STREQUAL "static" AND NOT _ncallers EQUAL 0))
    set(_ok FALSE)
  endif()

  set(_probe "${CMAKE_BINARY_DIR}/CMakeFiles/kickos-toolchain-newlib-probe-${_label}.c")
  if(_reent STREQUAL "dynamic")
    set(_reent_check "#if !defined(__DYNAMIC_REENT__)\n#error KICKOS_PROBE_STATIC_REENT\n#endif\n")
  else()
    set(_reent_check "#if defined(__DYNAMIC_REENT__)\n#error KICKOS_PROBE_DYNAMIC_REENT\n#endif\n")
  endif()
  file(WRITE "${_probe}" "#include <sys/reent.h>\n${_reent_check}${_nano_check}int kickos_probe;\n")
  separate_arguments(_link_list UNIX_COMMAND "${_link}")
  execute_process(COMMAND "${_cc}" ${_flags} ${_link_list} -fsyntax-only "${_probe}"
                  RESULT_VARIABLE _hrc OUTPUT_QUIET ERROR_VARIABLE _herr)
  if(NOT _ok OR NOT _hrc EQUAL 0)
    string(STRIP "${_herr}" _herr)
    message(FATAL_ERROR "KickOS ${_label} toolchain: the ${_flavor} newlib in ${_path} is not "
      "${_reent}-reent: ${_ncallers} member(s) call __getreent, ${_ndefiners} define it, and "
      "the header probe answered '${_herr}'")
  endif()
  set(KICKOS_TOOLCHAIN_NEWLIB_OK_${_label} "${_stamp}" CACHE INTERNAL "")
  message(STATUS "KickOS ${_label} toolchain: ${_flavor} ${_reent}-reent newlib from ${_path}")
endfunction()
