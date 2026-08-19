# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Shared capability gate for the ARM + RISC-V cross toolchain files: refuse at
# configure time a resolved compiler that cannot build KickOS. find_program HINTS
# fall through to PATH when the hinted directory is absent, and distro cross gccs
# there (Debian's arm-none-eabi, riscv64-unknown-elf) are C-only/picolibc: without
# this gate configure succeeds and the build dies much later on
# `#include <exception>` or at the application link.
#
# No link-test, deliberately: a bare-metal link needs the board's linker script +
# startup, supplied only at the application-link step (why the toolchain files set
# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY). Compiler queries plus one
# compile-only probe; nothing is linked.
#
# Installed MCU packages ship this file beside the toolchain file that includes
# it (see the root CMakeLists install); the include path is list-dir-relative.

# kickos_require_usable_cross_cxx(<label> <cxx> <override-var> <tarball-url> <flags>...)
#
#   <label>        family name for the diagnostic ("arm", "riscv")
#   <cxx>          the resolved C++ compiler. libstdc++ is a C++ question, and
#                  the C/ASM drivers come from the same install.
#   <override-var> the cache variable that repoints the search at a good install
#   <tarball-url>  the official build KickOS CI pins (.github/workflows/ci.yml)
#   <flags>...     the multilib-selecting flags THIS build compiles with
#                  (-mcpu/-mfpu/-mfloat-abi/-mthumb; -march/-mabi). Required: a
#                  toolchain can ship libstdc++ for its default multilib and not
#                  for the one this board selects.
function(kickos_require_usable_cross_cxx _label _cxx _override_var _tc_url)
  set(_flags ${ARGN})
  string(REPLACE ";" " " _flags_text "${_flags}")

  # CMake reads a toolchain file several times per configure (and once more per
  # try_compile), so the verdict is stamped once per (compiler, flags) pair.
  # Swapping either re-probes; replacing a toolchain IN PLACE at the same path
  # does not (the usual "wipe the build dir" caveat).
  set(_stamp "${_cxx}|${_flags_text}")
  if(DEFINED CACHE{KICKOS_CXX_RUNTIME_OK_${_label}}
     AND "$CACHE{KICKOS_CXX_RUNTIME_OK_${_label}}" STREQUAL "${_stamp}")
    return()
  endif()

  # Which multilib do these flags select? Diagnostic only: a compiler that
  # cannot answer is not fatal here; the library probe below is the verdict.
  execute_process(COMMAND "${_cxx}" ${_flags} -print-multi-directory
                  OUTPUT_VARIABLE _multilib OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0 OR _multilib STREQUAL "")
    set(_multilib "<compiler would not say>")
  endif()

  # gcc -print-file-name=<lib> answers an absolute path when the library exists in
  # the search dirs these flags select, and echoes the bare name back when not.
  # The full-C++ opt-in links both libs; a toolchain lacking them lacks the C++
  # headers too.
  set(_absent "")
  foreach(_lib libstdc++.a libsupc++.a)
    execute_process(COMMAND "${_cxx}" ${_flags} "-print-file-name=${_lib}"
                    OUTPUT_VARIABLE _path OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT IS_ABSOLUTE "${_path}" OR NOT EXISTS "${_path}")
      list(APPEND _absent "${_lib}")
    endif()
  endforeach()

  # Which libc? KickOS wants newlib. Test __PICOLIBC__ POSITIVELY: picolibc also
  # defines __NEWLIB__ for source compatibility, so "no picolibc macro" is the only
  # honest reading of "newlib". -fsyntax-only: no object, no link, so it holds
  # under CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY. A libc header carries the
  # macro, hence the include.
  set(_probe "${CMAKE_BINARY_DIR}/CMakeFiles/kickos-libc-probe-${_label}.cc")
  file(WRITE "${_probe}"
    "/* KickOS libc probe: compiled, never linked. */\n"
    "#include <stdlib.h>\n"
    "#if defined(__PICOLIBC__)\n"
    "#error KICKOS_PROBE_SAW_PICOLIBC\n"
    "#endif\n")
  execute_process(COMMAND "${_cxx}" ${_flags} -fsyntax-only "${_probe}"
                  RESULT_VARIABLE _rc
                  OUTPUT_VARIABLE _probe_out ERROR_VARIABLE _probe_err)
  set(_picolibc FALSE)
  set(_probe_broken "")
  if(NOT _rc EQUAL 0)
    if("${_probe_out}${_probe_err}" MATCHES "KICKOS_PROBE_SAW_PICOLIBC")
      set(_picolibc TRUE)
    else()
      # Not picolibc: the probe itself would not compile, so this compiler cannot
      # preprocess a plain libc header. Report that rather than guess a libc.
      string(STRIP "${_probe_out}${_probe_err}" _probe_broken)
    endif()
  endif()

  set(_why "")
  if(_absent)
    string(REPLACE ";" " and " _absent_text "${_absent}")
    string(APPEND _why
      "\n  * ${_absent_text} absent for multilib '${_multilib}'"
      " (-print-file-name echoed the bare name back instead of a path)")
  endif()
  if(_picolibc)
    # If picolibc is ever adopted, delete this block; nothing else reads _picolibc.
    string(APPEND _why
      "\n  * its libc is PICOLIBC (__PICOLIBC__ is defined); KickOS requires NEWLIB")
  endif()
  if(_probe_broken)
    string(APPEND _why
      "\n  * it cannot compile `#include <stdlib.h>` at all, so it has no usable"
      " libc headers:\n      ${_probe_broken}")
  endif()

  if(_why STREQUAL "")
    set(KICKOS_CXX_RUNTIME_OK_${_label} "${_stamp}"
        CACHE INTERNAL "Cross C++ capability gate: (compiler|flags) already proven usable")
    message(STATUS "KickOS ${_label} toolchain: ${_cxx} usable "
                   "(libstdc++ + libsupc++ present for multilib '${_multilib}')")
    return()
  endif()

  message(FATAL_ERROR
    "KickOS ${_label} toolchain: the compiler that was found cannot build KickOS.\n"
    "  compiler : ${_cxx}\n"
    "  flags    : ${_flags_text}\n"
    "problem(s):${_why}\n"
    "This is a PATH fall-through: ${_override_var} is unset (or names a directory that "
    "does not exist on this host), so find_program kept searching and settled on a "
    "distro cross gcc that is C-only (no libstdc++/libsupc++) and/or picolibc-based. "
    "Nothing here is wrong with your checkout.\n"
    "Fix: install the official toolchain KickOS builds with and point the build at "
    "its bin/ directory, by either route:\n"
    "  ${_tc_url}\n"
    "  cmake --preset <preset> -D${_override_var}=/path/to/toolchain/bin\n"
    "  export ${_override_var}=/path/to/toolchain/bin   # or put that bin/ on PATH\n"
    "Reuse of an existing build dir keeps the old cached compiler: reconfigure into a "
    "fresh one.")
endfunction()
