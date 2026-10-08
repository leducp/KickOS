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
# The third field is the name a per-preset gate is REGISTERED under, cmake/preset_key.cmake's
# from the resolved facts, reading the same defconfig CMakeLists resolves the posture from;
# that is what a `preset` record in console_reach_roots.txt is matched against, and it is not
# the preset name.

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

    # A string carries no type, which CMake caches as UNINITIALIZED; a boolean is a BOOL; an
    # object states its own. A null unsets what a parent would give.
    set(_own_${_key} "")
    string(JSON _ncv ERROR_VARIABLE _ignored LENGTH "${_p}" cacheVariables)
    if(_ncv)
      math(EXPR _ncvlast "${_ncv} - 1")
      foreach(_j RANGE 0 ${_ncvlast})
        string(JSON _cv MEMBER "${_p}" cacheVariables ${_j})
        if(NOT _cv MATCHES "^KICKOS_[A-Z0-9_]+$")
          continue()
        endif()
        string(JSON _t TYPE "${_p}" cacheVariables ${_cv})
        set(_ty "UNINITIALIZED")
        set(_val "")
        if(_t STREQUAL "NULL")
          set(_ty "")
        elseif(_t STREQUAL "BOOLEAN")
          set(_ty "BOOL")
          string(JSON _b GET "${_p}" cacheVariables ${_cv})
          set(_val "FALSE")
          if(_b)
            set(_val "TRUE")
          endif()
        elseif(_t STREQUAL "OBJECT")
          string(JSON _b ERROR_VARIABLE _noty GET "${_p}" cacheVariables ${_cv} type)
          if(NOT _noty)
            set(_ty "${_b}")
          endif()
          string(JSON _vt ERROR_VARIABLE _ignored TYPE "${_p}" cacheVariables ${_cv} value)
          string(JSON _val ERROR_VARIABLE _ignored GET "${_p}" cacheVariables ${_cv} value)
          if(_vt STREQUAL "BOOLEAN")
            set(_b "${_val}")
            set(_val "FALSE")
            if(_b)
              set(_val "TRUE")
            endif()
          endif()
        else()
          string(JSON _val GET "${_p}" cacheVariables ${_cv})
        endif()
        list(APPEND _own_${_key} "${_cv}")
        set(_cvt_${_cv}_${_key} "${_ty}")
        set(_cvv_${_cv}_${_key} "${_val}")
      endforeach()
    endif()
  endforeach()
endforeach()

if(NOT _names)
  message(FATAL_ERROR "preset_boards.cmake: no configure preset in ${SRC}/CMakePresets.json")
endif()

include("${SRC}/cmake/preset_key.cmake")

# The preset's own value, else the first parent in `inherits` order that resolves one; a null
# resolves, to unset. Sets _r_type ("" when unset) and _r_value in the caller.
function(_resolve name var)
  string(MAKE_C_IDENTIFIER "${name}" _k)
  if("${var}" IN_LIST _own_${_k})
    set(_r_type "${_cvt_${var}_${_k}}" PARENT_SCOPE)
    set(_r_value "${_cvv_${var}_${_k}}" PARENT_SCOPE)
    set(_r_found 1 PARENT_SCOPE)
    return()
  endif()
  foreach(_parent IN LISTS _inh_${_k})
    _resolve("${_parent}" "${var}")
    if(_r_found)
      set(_r_type "${_r_type}" PARENT_SCOPE)
      set(_r_value "${_r_value}" PARENT_SCOPE)
      set(_r_found 1 PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(_r_type "" PARENT_SCOPE)
  set(_r_value "" PARENT_SCOPE)
  set(_r_found 0 PARENT_SCOPE)
endfunction()

set(_table "")
foreach(_name IN LISTS _names)
  string(MAKE_C_IDENTIFIER "${_name}" _key)
  if(_hidden_${_key})
    continue()
  endif()
  foreach(_cv KICKOS_BOARD KICKOS_CONFIG_VARIANT KICKOS_AMP_NODE_ID)
    set(_cv_${_cv} "")
    _resolve("${_name}" "${_cv}")
    if(NOT _r_type STREQUAL "")
      set(_cv_${_cv} "${_r_value}")
    endif()
  endforeach()
  set(_b "${_cv_KICKOS_BOARD}")
  if("${_b}" STREQUAL "")
    set(_b "@none")
  endif()
  # "base" is the root CMakeLists' own default for an unstated variant.
  set(_variant "${_cv_KICKOS_CONFIG_VARIANT}")
  if("${_variant}" STREQUAL "")
    set(_variant "base")
  endif()
  set(_own_image OFF)
  set(_dc "${SRC}/boards/${_b}/configs/${_variant}/defconfig")
  if(EXISTS "${_dc}")
    file(STRINGS "${_dc}" _own REGEX "^CONFIG_KICKOS_AMP_POSTURE_OWN_IMAGE=y$")
    if(NOT "${_own}" STREQUAL "")
      set(_own_image ON)
    endif()
  endif()
  set(_nid "${_cv_KICKOS_AMP_NODE_ID}")
  if("${_nid}" STREQUAL "")
    set(_nid "0")
  endif()
  kickos_preset_key(_regkey "${_b}" "${_variant}" "${_own_image}" "${_nid}")
  string(APPEND _table "${_name}\t${_b}\t${_regkey}\n")
endforeach()

file(WRITE "${OUT}" "${_table}")
