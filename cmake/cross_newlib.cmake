# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The dynamic-reent newlib an image links in place of the toolchain's own (conan/newlib).
#
# The toolchain's newlib stays on disk and in its include path, so it is REMOVED from the
# search: -nostdinc, then the compiler's own system directories back in their order with the
# toolchain's libc headers replaced by the package's. An -isystem ahead of them instead would
# still hand every libstdc++ `#include_next <stdlib.h>` the stock header.
#
# The package's lib/ goes first on the link line, so every -lc the rescan group and the driver
# add resolves there; a configure-time link trace proves that it does.

# kickos_require_dynreent_newlib(<label> <cc> <cxx> <env-var> <flags>...)
#
# Sets _kos_newlib_c, _kos_newlib_cxx and _kos_newlib_link in the caller.
function(kickos_require_dynreent_newlib _label _cc _cxx _var)
  set(_flags ${ARGN})
  set(${_var} "$ENV{${_var}}" CACHE PATH
      "The kickos-newlib package folder (include/, lib/) this multilib links")
  set(ENV{${_var}} "${${_var}}")
  set(_dir "${${_var}}")
  # conan/board names each multilib's variable KICKOS_NEWLIB_<MULTILIB>.
  string(REGEX REPLACE "^KICKOS_NEWLIB_" "" _multilib "${_var}")
  string(TOLOWER "${_multilib}" _multilib)
  string(CONCAT _how "Provision it from the source root with\n"
         "  conan export conan/newlib\n"
         "  conan install conan/board -o \"&:multilib=${_multilib}\" --build=missing "
         "--output-folder=<dir>\n"
         "  source <dir>/kickos-newlib-${_multilib}.sh\n"
         "and configure again in that shell.")
  if(_dir STREQUAL "")
    message(FATAL_ERROR
      "KickOS ${_label} toolchain: ${_var} is unset. Arch '${KICKOS_ARCH}' links a newlib whose "
      "libc reaches its reentrant state through __getreent(), which the runtime answers per "
      "thread; the toolchain's own libc reads one shared _impure_ptr instead. ${_how}")
  endif()

  # Ahead of the stamp: a cached folder can be deleted or emptied between configures.
  foreach(_need include/newlib.h include/sys/reent.h lib/libc.a lib/libm.a kickos-newlib.txt)
    if(NOT EXISTS "${_dir}/${_need}")
      message(FATAL_ERROR
        "KickOS ${_label} toolchain: ${_var}=${_dir} has no ${_need}, so it is not a "
        "kickos-newlib package. ${_how}")
    endif()
  endforeach()

  set(_stamp "${_dir}|${_cxx}|${_flags}")
  if(DEFINED CACHE{KICKOS_NEWLIB_OK_${_label}}
     AND "$CACHE{KICKOS_NEWLIB_OK_${_label}}" STREQUAL "${_stamp}")
    message(STATUS "KickOS ${_label} toolchain: dynamic-reent newlib from ${_dir}")
    set(_kos_newlib_c "$CACHE{KICKOS_NEWLIB_C_${_label}}" PARENT_SCOPE)
    set(_kos_newlib_cxx "$CACHE{KICKOS_NEWLIB_CXX_${_label}}" PARENT_SCOPE)
    set(_kos_newlib_link "$CACHE{KICKOS_NEWLIB_LINK_${_label}}" PARENT_SCOPE)
    return()
  endif()

  file(STRINGS "${_dir}/kickos-newlib.txt" _pkg_flags REGEX "^flags=")
  string(REGEX REPLACE "^flags=" "" _pkg_flags "${_pkg_flags}")
  separate_arguments(_pkg_flags UNIX_COMMAND "${_pkg_flags}")
  execute_process(COMMAND "${_cc}" ${_pkg_flags} -print-multi-directory
                  OUTPUT_VARIABLE _pkg_multi OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND "${_cc}" ${_flags} -print-multi-directory
                  OUTPUT_VARIABLE _our_multi OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT _pkg_multi STREQUAL _our_multi)
    message(FATAL_ERROR
      "KickOS ${_label} toolchain: ${_var}=${_dir} was built for multilib '${_pkg_multi}' and "
      "this board selects '${_our_multi}'. ${_how}")
  endif()

  # Dynamic reentrancy is a property of both halves: headers that expand _REENT to the hook,
  # and a libc.a whose members call it and that defines none.
  get_filename_component(_bin "${_cc}" DIRECTORY)
  get_filename_component(_cc_name "${_cc}" NAME)
  string(REGEX REPLACE "gcc$" "nm" _nm_name "${_cc_name}")
  execute_process(COMMAND "${_bin}/${_nm_name}" -A "${_dir}/lib/libc.a"
                  OUTPUT_VARIABLE _nm ERROR_QUIET RESULT_VARIABLE _rc)
  string(REGEX MATCHALL " U __getreent\n" _callers "${_nm}")
  string(REGEX MATCHALL " [TtWw] __getreent\n" _definers "${_nm}")
  list(LENGTH _callers _ncallers)
  list(LENGTH _definers _ndefiners)

  # The toolchain's libc headers are found through the directory holding its newlib.h.
  execute_process(COMMAND "${_cc}" ${_flags} -print-file-name=libc.a
                  OUTPUT_VARIABLE _stock_libc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  get_filename_component(_walk "${_stock_libc}" DIRECTORY)
  set(_stock_inc "")
  while(NOT _walk STREQUAL "/" AND _stock_inc STREQUAL "")
    if(EXISTS "${_walk}/include/newlib.h")
      get_filename_component(_stock_inc "${_walk}/include" REALPATH)
      get_filename_component(_stock_sys "${_walk}/sys-include" REALPATH)
    endif()
    get_filename_component(_walk "${_walk}" DIRECTORY)
  endwhile()
  if(_stock_inc STREQUAL "")
    message(FATAL_ERROR
      "KickOS ${_label} toolchain: no newlib.h above ${_stock_libc}, so the toolchain's own "
      "libc headers cannot be taken out of the search.")
  endif()

  foreach(_key c cxx)
    set(_lang "${_key}")
    if(_key STREQUAL "cxx")
      set(_lang "c++")
    endif()
    execute_process(COMMAND "${_cxx}" ${_flags} -x ${_lang} -E -v -
                    INPUT_FILE /dev/null OUTPUT_QUIET ERROR_VARIABLE _v)
    string(REGEX MATCH "#include <\\.\\.\\.> search starts here:\n(.*)\nEnd of search list" _m
           "${_v}")
    string(REGEX REPLACE "\n" ";" _dirs "${CMAKE_MATCH_1}")
    set(_out "-nostdinc")
    set(_swapped FALSE)
    foreach(_d IN LISTS _dirs)
      string(STRIP "${_d}" _d)
      get_filename_component(_real "${_d}" REALPATH)
      if(_real STREQUAL _stock_inc OR _real STREQUAL _stock_sys)
        if(NOT _swapped)
          string(APPEND _out " -isystem ${_dir}/include")
          set(_swapped TRUE)
        endif()
      else()
        string(APPEND _out " -isystem ${_real}")
      endif()
    endforeach()
    if(NOT _swapped)
      message(FATAL_ERROR
        "KickOS ${_label} toolchain: ${_stock_inc} is not in the ${_lang} search list of "
        "${_cxx}, so the package's headers have no place to take.")
    endif()
    set(_inc_${_key} "${_out}")
  endforeach()

  set(_probe "${CMAKE_BINARY_DIR}/CMakeFiles/kickos-newlib-probe-${_label}.c")
  file(WRITE "${_probe}"
    "#include <sys/reent.h>\n#include <errno.h>\n"
    "#if !defined(__DYNAMIC_REENT__)\n#error KICKOS_PROBE_STATIC_REENT\n#endif\n"
    "int kickos_newlib_probe(void) { return errno + (_REENT != 0); }\n")
  separate_arguments(_c_list UNIX_COMMAND "${_inc_c}")
  execute_process(COMMAND "${_cc}" ${_flags} ${_c_list} -fsyntax-only "${_probe}"
                  RESULT_VARIABLE _hrc OUTPUT_QUIET ERROR_VARIABLE _herr)
  if(NOT _rc EQUAL 0 OR _ncallers EQUAL 0 OR NOT _ndefiners EQUAL 0 OR NOT _hrc EQUAL 0)
    string(STRIP "${_herr}" _herr)
    message(FATAL_ERROR
      "KickOS ${_label} toolchain: ${_var}=${_dir} is not a dynamic-reent newlib: "
      "${_ncallers} libc.a member(s) call __getreent, ${_ndefiners} define it, and the header "
      "probe answered '${_herr}'. ${_how}")
  endif()

  set(_link "-L${_dir}/lib")
  set(_trace_obj "${CMAKE_BINARY_DIR}/CMakeFiles/kickos-newlib-probe-${_label}.o")
  execute_process(COMMAND "${_cc}" ${_flags} ${_c_list} ${_link} -nostdlib -Wl,-r
                          -Wl,--trace "${_probe}" -lc -o "${_trace_obj}"
                  OUTPUT_VARIABLE _trace ERROR_VARIABLE _trace_err RESULT_VARIABLE _lrc)
  string(REGEX MATCHALL "[^\n]*libc\\.a[^\n]*" _libcs "${_trace}")
  if(NOT _lrc EQUAL 0 OR NOT _libcs STREQUAL "${_dir}/lib/libc.a")
    message(FATAL_ERROR
      "KickOS ${_label} toolchain: a link with -L${_dir}/lib did not take libc.a from there:\n"
      "${_trace}${_trace_err}")
  endif()
  file(REMOVE "${_trace_obj}")

  set(KICKOS_NEWLIB_C_${_label} "${_inc_c}" CACHE INTERNAL "")
  set(KICKOS_NEWLIB_CXX_${_label} "${_inc_cxx}" CACHE INTERNAL "")
  set(KICKOS_NEWLIB_LINK_${_label} "${_link}" CACHE INTERNAL "")
  set(KICKOS_NEWLIB_OK_${_label} "${_stamp}" CACHE INTERNAL "")
  message(STATUS "KickOS ${_label} toolchain: dynamic-reent newlib from ${_dir}")
  set(_kos_newlib_c "${_inc_c}" PARENT_SCOPE)
  set(_kos_newlib_cxx "${_inc_cxx}" PARENT_SCOPE)
  set(_kos_newlib_link "${_link}" PARENT_SCOPE)
endfunction()
