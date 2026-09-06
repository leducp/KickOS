# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Flattens the CMakePresets tree into one TAB-separated line per VISIBLE configure
# preset:
#
#   <preset> <TAB> <KICKOS_BOARD> <TAB> <registration key>
#
# Run as: cmake -DSRC=<repo root> -DOUT=<file> -P tests/static/preset_boards.cmake
#
# KICKOS_BOARD is set on a handful of base presets and INHERITED by every -st, -flat,
# -telem and -bench variant, so a line-shaped scan finds a board for a third of the file
# and none for the rest. The inherit walk below is where a variant's board comes from,
# and KICKOS_CONFIG_VARIANT and KICKOS_AMP_NODE_ID come the same way.
#
# A preset whose board never resolves is emitted as @none rather than dropped: the
# caller has to tell "this preset names no board" from "this preset is not there at
# all", and a dropped line makes those identical.
#
# The third field is the name a per-preset gate is REGISTERED under. CMake names no
# preset, so the root CMakeLists rebuilds one out of the board, the variant and, on an
# own-image AMP node, the node index; that is what a `preset` record in
# trap_redzone_roots.txt and console_reach_roots.txt is matched against, and it is not
# the preset name. The rebuild here restates the one in CMakeLists.txt so the two can be
# compared, and reads the same defconfig CMakeLists resolves the posture from.

cmake_minimum_required(VERSION 3.24)

foreach(_v SRC OUT)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "preset_boards.cmake: -D${_v}=<path> is required")
  endif()
endforeach()

# The root file plus everything it includes, transitively. An include path is relative
# to the file that names it, not to the root.
set(_queue "${SRC}/CMakePresets.json")
set(_files "")
while(_queue)
  list(POP_FRONT _queue _f)
  if("${_f}" IN_LIST _files)
    continue()
  endif()
  if(NOT EXISTS "${_f}")
    message(FATAL_ERROR "preset_boards.cmake: no such preset file: ${_f}")
  endif()
  list(APPEND _files "${_f}")
  file(READ "${_f}" _json)
  get_filename_component(_dir "${_f}" DIRECTORY)
  string(JSON _n ERROR_VARIABLE _ignored LENGTH "${_json}" include)
  if(_n)
    math(EXPR _last "${_n} - 1")
    foreach(_i RANGE 0 ${_last})
      string(JSON _inc GET "${_json}" include ${_i})
      if(NOT IS_ABSOLUTE "${_inc}")
        set(_inc "${_dir}/${_inc}")
      endif()
      list(APPEND _queue "${_inc}")
    endforeach()
  endif()
endwhile()

# The cache variables the registration key is built from, each resolved by the inherit walk.
set(_cachevars KICKOS_BOARD KICKOS_CONFIG_VARIANT KICKOS_AMP_NODE_ID)

set(_names "")
foreach(_f IN LISTS _files)
  file(READ "${_f}" _json)
  string(JSON _n ERROR_VARIABLE _ignored LENGTH "${_json}" configurePresets)
  if(NOT _n)
    continue()
  endif()
  math(EXPR _last "${_n} - 1")
  foreach(_i RANGE 0 ${_last})
    string(JSON _p GET "${_json}" configurePresets ${_i})
    string(JSON _name GET "${_p}" name)
    string(MAKE_C_IDENTIFIER "${_name}" _key)
    if("${_name}" IN_LIST _names)
      message(FATAL_ERROR "preset_boards.cmake: two configure presets named '${_name}'")
    endif()
    list(APPEND _names "${_name}")

    set(_hidden_${_key} 0)
    string(JSON _h ERROR_VARIABLE _ignored GET "${_p}" hidden)
    if(_h)
      set(_hidden_${_key} 1)
    endif()

    # `inherits` is a string or an array, and the array's ORDER is its precedence.
    set(_inh_${_key} "")
    string(JSON _t ERROR_VARIABLE _ignored TYPE "${_p}" inherits)
    if(_t STREQUAL "STRING")
      string(JSON _one GET "${_p}" inherits)
      set(_inh_${_key} "${_one}")
    elseif(_t STREQUAL "ARRAY")
      string(JSON _ni LENGTH "${_p}" inherits)
      math(EXPR _nilast "${_ni} - 1")
      foreach(_j RANGE 0 ${_nilast})
        string(JSON _one GET "${_p}" inherits ${_j})
        list(APPEND _inh_${_key} "${_one}")
      endforeach()
    endif()

    foreach(_cv IN LISTS _cachevars)
      set(_cv_${_cv}_${_key} "")
      string(JSON _b ERROR_VARIABLE _ignored GET "${_p}" cacheVariables ${_cv})
      if(NOT _ignored)
        set(_cv_${_cv}_${_key} "${_b}")
      endif()
    endforeach()
  endforeach()
endforeach()

if(NOT _names)
  message(FATAL_ERROR "preset_boards.cmake: no configure preset in ${SRC}/CMakePresets.json")
endif()

# Fixpoint rather than recursion: one pass can only propagate a value one level, so
# repeat until nothing moves. Bounded by the preset count, which also makes an
# inherit CYCLE terminate (its members simply keep no value and come out empty).
list(LENGTH _names _rounds)
foreach(_cv IN LISTS _cachevars)
  foreach(_r RANGE 1 ${_rounds})
    set(_moved 0)
    foreach(_name IN LISTS _names)
      string(MAKE_C_IDENTIFIER "${_name}" _key)
      if(NOT "${_cv_${_cv}_${_key}}" STREQUAL "")
        continue()
      endif()
      foreach(_parent IN LISTS _inh_${_key})
        string(MAKE_C_IDENTIFIER "${_parent}" _pkey)
        if(NOT "${_cv_${_cv}_${_pkey}}" STREQUAL "")
          set(_cv_${_cv}_${_key} "${_cv_${_cv}_${_pkey}}")
          set(_moved 1)
          break()
        endif()
      endforeach()
    endforeach()
    if(NOT _moved)
      break()
    endif()
  endforeach()
endforeach()

set(_table "")
foreach(_name IN LISTS _names)
  string(MAKE_C_IDENTIFIER "${_name}" _key)
  if(_hidden_${_key})
    continue()
  endif()
  set(_b "${_cv_KICKOS_BOARD_${_key}}")
  if("${_b}" STREQUAL "")
    set(_b "@none")
  endif()
  # "base" is the root CMakeLists' own default for an unstated variant.
  set(_variant "${_cv_KICKOS_CONFIG_VARIANT_${_key}}")
  if("${_variant}" STREQUAL "")
    set(_variant "base")
  endif()
  set(_regkey "${_b}")
  if(NOT _variant STREQUAL "base")
    set(_regkey "${_b}-${_variant}")
  endif()
  # The node index enters the key on the own-image posture alone, which is where one
  # partition has one preset per node. Keying on KICKOS_AMP_NODE_ID being set instead
  # would suffix every AMP preset, the shared-image posture carrying a Kconfig default
  # for it.
  set(_dc "${SRC}/boards/${_b}/configs/${_variant}/defconfig")
  if(EXISTS "${_dc}")
    file(STRINGS "${_dc}" _own REGEX "^CONFIG_KICKOS_AMP_POSTURE_OWN_IMAGE=y$")
    if(NOT "${_own}" STREQUAL "")
      set(_nid "${_cv_KICKOS_AMP_NODE_ID_${_key}}")
      if("${_nid}" STREQUAL "")
        set(_nid "0")
      endif()
      set(_regkey "${_regkey}-n${_nid}")
    endif()
  endif()
  string(APPEND _table "${_name}\t${_b}\t${_regkey}\n")
endforeach()

file(WRITE "${OUT}" "${_table}")
