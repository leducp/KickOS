# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Locating the Kconfig interpreter. Kconfig owns configuration and CMake owns the
# build graph, so nothing here decides a knob's value: it finds the interpreter that
# resolves them and reports whether kconfiglib is importable.

# Hands back the interpreter and whether it can import kconfiglib.
#
# KICKOS_KCONFIG_PY comes from the environment as an ABSOLUTE path, because
# kconfiglib belongs in a venv of its own: that venv's bin/ holds python and
# python3, and putting it on PATH shadows the toolchain interpreters the ESP flash
# tools need. Never add it to PATH; name the interpreter.
function(kickos_find_kconfig out_python out_found)
  set(_py "")
  if(DEFINED ENV{KICKOS_KCONFIG_PY})
    set(_py "$ENV{KICKOS_KCONFIG_PY}")
  endif()
  if(NOT _py)
    # NO_CACHE: find_program otherwise leaves a cache entry named after this local,
    # which then outlives a later KICKOS_KCONFIG_PY appearing in the environment.
    find_program(_py NAMES python3 python NO_CACHE)
  endif()

  set(${out_found} FALSE PARENT_SCOPE)
  set(${out_python} "${_py}" PARENT_SCOPE)
  if(NOT _py OR NOT EXISTS "${_py}")
    return()
  endif()

  execute_process(COMMAND "${_py}" -c "import kconfiglib"
                  RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
  if(NOT _rc EQUAL 0)
    return()
  endif()
  set(${out_found} TRUE PARENT_SCOPE)

  # The stock entry points, as SCRIPTS run by that interpreter rather than as programs on
  # PATH: this venv's bin/ holds python and python3, and putting it on PATH shadows the
  # interpreter the ESP flash tools need. Absent from a bare pip install of the module,
  # so each is optional and the caller must test it.
  get_filename_component(_bin "${_py}" DIRECTORY)
  foreach(_tool menuconfig savedefconfig)
    string(TOUPPER "${_tool}" _upper)
    if(EXISTS "${_bin}/${_tool}")
      set(KICKOS_KCONFIG_BIN_${_upper} "${_bin}/${_tool}" PARENT_SCOPE)
    endif()
  endforeach()
endfunction()

# Resolves the tree for one board+variant into gendir. Refuses rather than falling
# back: a board with a defconfig is configured from Kconfig, and nothing else.
function(kickos_kconfig_generate srcdir defconfig gendir overrides)
  kickos_find_kconfig(_py _found)
  if(NOT _found)
    kickos_kconfig_hint(_hint)
    message(FATAL_ERROR
      "Board '${KICKOS_BOARD}' is configured from Kconfig (${defconfig}), so the "
      "generator is required. ${_hint}")
  endif()
  execute_process(
    COMMAND "${_py}" "${srcdir}/tools/kconfig/genconfig.py"
            "${srcdir}" "${defconfig}" "${gendir}" ${overrides}
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
      "KickOS: the configuration was REFUSED. Every requested value is read back "
      "after resolution, so this is a value the declarations do not permit rather "
      "than a tool failure.\n${_err}${_out}")
  endif()
  if(_err)
    # kconfiglib warns on stderr and keeps going. A warning here means a declaration
    # is wrong even though the resolution succeeded.
    message(WARNING "KickOS: kconfig warnings:\n${_err}")
  endif()
endfunction()

# For the facts that genuinely have two independent sources. A value the fragment
# merely REPLACES has nothing to compare against and must not be checked here: an
# assert against a number you just supplied is a tautology dressed as a guarantee.
function(kickos_kconfig_agree what cmake_value kconfig_value)
  if(NOT "${cmake_value}" STREQUAL "${kconfig_value}")
    message(FATAL_ERROR
      "KickOS: Kconfig and CMake disagree on ${what}: .config resolves "
      "'${kconfig_value}', the CMake path computes '${cmake_value}'. One of the two "
      "is wrong and they are not interchangeable.")
  endif()
endfunction()

# The diagnostic for a user who has no kconfiglib.
function(kickos_kconfig_hint out)
  string(CONCAT _hint
    "kconfiglib is not importable. It is build-time only and one pure-Python file, "
    "and it belongs in a venv of its own: "
    "python3 -m venv <dir> && <dir>/bin/pip install kconfiglib==14.1.0, then "
    "export KICKOS_KCONFIG_PY=<dir>/bin/python. That must be an ABSOLUTE path and "
    "its bin/ must stay off PATH, or it shadows the interpreter the ESP flash tools "
    "run on. A packaged kconfig-mconf drives menuconfig, but not the generator.")
  set(${out} "${_hint}" PARENT_SCOPE)
endfunction()
