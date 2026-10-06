# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A packaged driver's metadata as kickos_add_driver takes it, validated and rendered into the
# catalogue entry the manifest carries and the header its descriptor reads. Script-safe: it
# creates no target, so tests/static/check_driver_metadata.cmake drives it under cmake -P.

set(KICKOS_DRIVER_OPTIONS NOTIFY CONSOLE)
set(KICKOS_DRIVER_SINGLE CLASS REGDIR BLOCK BLOCK_CACHE POSTURE BARRIER START RECEIVER)
set(KICKOS_DRIVER_MULTI SOURCES THREADS WINDOWS LINES CLIENT)
set(KICKOS_DRIVER_METADATA THREADS WINDOWS LINES BLOCK BLOCK_CACHE POSTURE BARRIER START RECEIVER NOTIFY CONSOLE
    CLIENT)

# Writes `content` to `path` unless it already holds it, so nothing that depends on the file is
# rebuilt for an identical configure. No @-reference or ${} in `content` is expanded again.
function(kickos_write_if_changed path content)
  if(EXISTS "${path}")
    file(READ "${path}" _old)
    if(_old STREQUAL content)
      return()
    endif()
  endif()
  file(WRITE "${path}" "${content}")
endfunction()

function(_kickos_json_quote value out)
  string(REPLACE "\\" "\\\\" value "${value}")
  string(REPLACE "\"" "\\\"" value "${value}")
  set(${out} "\"${value}\"" PARENT_SCOPE)
endfunction()

# The JSON array of `items`, each already a JSON value.
function(_kickos_json_array out)
  list(JOIN ARGN ", " _body)
  set(${out} "[${_body}]" PARENT_SCOPE)
endfunction()

# kickos_driver_metadata(<name> <out_packaged> <out_json> <out_header> <kickos_add_driver args>...)
#   <out_packaged> is TRUE when the arguments declare a packaged driver, which THREADS does; the
#   other two are then its catalogue entry as JSON and its <kickos/driver/declared/<name>.h>.
#   Every value is tested by DEFINED or by string, never by if(<value>): a role may be named
#   `off` or `no`, which CMake reads as false.
function(kickos_driver_metadata name out_packaged out_json out_header)
  cmake_parse_arguments(M "${KICKOS_DRIVER_OPTIONS}" "${KICKOS_DRIVER_SINGLE}" "${KICKOS_DRIVER_MULTI}" ${ARGN})
  if(DEFINED M_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "kickos_add_driver(${name}): unknown arguments ${M_UNPARSED_ARGUMENTS}")
  endif()
  if(DEFINED M_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "kickos_add_driver(${name}): ${M_KEYWORDS_MISSING_VALUES} given with no value")
  endif()
  set(${out_packaged} FALSE PARENT_SCOPE)
  if(NOT DEFINED M_THREADS)
    foreach(_key IN LISTS KICKOS_DRIVER_METADATA)
      if(DEFINED M_${_key} AND NOT "${M_${_key}}" STREQUAL "FALSE")
        message(FATAL_ERROR "kickos_add_driver(${name}): ${_key} is packaged-driver metadata, "
          "and a driver declares it beside its THREADS or not at all")
      endif()
    endforeach()
    return()
  endif()
  foreach(_key BLOCK POSTURE BARRIER START RECEIVER)
    if(NOT DEFINED M_${_key})
      message(FATAL_ERROR "kickos_add_driver(${name}): a packaged driver declares ${_key}")
    endif()
  endforeach()
  foreach(_fact KICKOS_DRIVER_ENDPOINTS KICKOS_DRIVER_NOTIFICATIONS)
    if(NOT DEFINED ${_fact})
      message(FATAL_ERROR "kickos_add_driver(${name}): no ${_fact}; cmake/driver_geometry.cmake "
        "declares what the shared bring-up creates")
    endif()
  endforeach()
  if(NOT M_BLOCK STREQUAL "none" AND NOT M_BLOCK MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "kickos_add_driver(${name}): BLOCK is the ring block's size in bytes, "
      "or none, not '${M_BLOCK}'")
  endif()
  if(NOT M_BLOCK STREQUAL "none")
    math(EXPR _low "${M_BLOCK} & (${M_BLOCK} - 1)")
    if(NOT _low EQUAL 0)
      message(FATAL_ERROR "kickos_add_driver(${name}): BLOCK ${M_BLOCK} is no power of two, and the "
        "kernel grants a ring block only as one")
    endif()
  endif()
  if(NOT DEFINED M_BLOCK_CACHE)
    set(M_BLOCK_CACHE cached)
  endif()
  if(NOT M_BLOCK_CACHE MATCHES "^(cached|uncached)$")
    message(FATAL_ERROR "kickos_add_driver(${name}): BLOCK_CACHE is cached or uncached, not '${M_BLOCK_CACHE}'")
  endif()
  if(M_BLOCK_CACHE STREQUAL "uncached" AND M_BLOCK STREQUAL "none")
    message(FATAL_ERROR "kickos_add_driver(${name}): BLOCK_CACHE uncached types a ring block, and BLOCK is none")
  endif()
  if(NOT M_START MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR "kickos_add_driver(${name}): START is the C symbol the init calls to bring "
      "the driver up, not '${M_START}'")
  endif()
  if(NOT M_POSTURE MATCHES "^(handover|retain)$")
    message(FATAL_ERROR "kickos_add_driver(${name}): POSTURE is handover or retain, not '${M_POSTURE}'")
  endif()
  if(M_CONSOLE AND NOT M_POSTURE STREQUAL "handover")
    message(FATAL_ERROR "kickos_add_driver(${name}): a driver that takes the console hands its "
      "endpoint over, so its POSTURE is handover")
  endif()

  # A role is its thread's name, but the role `service`, whose thread takes its service-list
  # entry's name.
  set(_roles "")
  set(_prio "")
  set(_names "")
  set(_threads "")
  set(_caps "")
  set(_badged "")
  foreach(_t IN LISTS M_THREADS)
    if(NOT _t MATCHES "^([a-z][a-z0-9_]*):([0-9]+):(default):([0-9]+):([0-9]+)$" OR CMAKE_MATCH_2 GREATER 127
       OR CMAKE_MATCH_4 GREATER 255 OR CMAKE_MATCH_5 GREATER CMAKE_MATCH_4)
      message(FATAL_ERROR "kickos_add_driver(${name}): THREADS entry '${_t}' is not "
        "<role>:<priority offset 0 to 127>:default:<capabilities its spawn delegates>"
        ":<badged notification copies among them>")
    endif()
    list(APPEND _roles "${CMAKE_MATCH_1}")
    list(APPEND _prio "${CMAKE_MATCH_2}")
    list(APPEND _caps "${CMAKE_MATCH_4}")
    list(APPEND _badged "${CMAKE_MATCH_5}")
    if(CMAKE_MATCH_1 STREQUAL "service")
      list(APPEND _names "nullptr")
    else()
      list(APPEND _names "\"${CMAKE_MATCH_1}\"")
    endif()
    _kickos_json_quote("${CMAKE_MATCH_1}" _qrole)
    _kickos_json_quote("${CMAKE_MATCH_3}" _qstack)
    list(APPEND _threads "{\"name\": ${_qrole}, \"priority\": ${CMAKE_MATCH_2}, \"stack\": ${_qstack}, \
\"caps\": ${CMAKE_MATCH_4}, \"badged\": ${CMAKE_MATCH_5}}")
  endforeach()
  list(LENGTH _roles _thread_count)
  list(FIND _roles "${M_RECEIVER}" _receiver)
  if(_receiver EQUAL -1)
    message(FATAL_ERROR "kickos_add_driver(${name}): RECEIVER is the THREADS role that receives on "
      "the driver's endpoint, not '${M_RECEIVER}'")
  endif()
  set(_windows "")
  set(_lines "")
  foreach(_kind THREADS WINDOWS LINES)
    set(_list "${_roles}")
    if(NOT _kind STREQUAL "THREADS")
      set(_list "${M_${_kind}}")
    endif()
    set(_seen "")
    foreach(_r IN LISTS _list)
      if(NOT _r MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "kickos_add_driver(${name}): ${_kind} role '${_r}' is not a name "
          "of the form [a-z][a-z0-9_]*")
      endif()
      if(_r IN_LIST _seen)
        message(FATAL_ERROR "kickos_add_driver(${name}): ${_kind} names '${_r}' twice")
      endif()
      list(APPEND _seen "${_r}")
      _kickos_json_quote("${_r}" _q)
      if(_kind STREQUAL "WINDOWS")
        list(APPEND _windows "${_q}")
      elseif(_kind STREQUAL "LINES")
        list(APPEND _lines "${_q}")
      endif()
    endforeach()
  endforeach()
  if(M_BARRIER STREQUAL "none")
    set(_barrier_after ${_thread_count})
    set(_qbarrier "\"none\"")
  elseif(M_BARRIER MATCHES "^[1-9][0-9]*$" AND NOT M_BARRIER GREATER _thread_count
         AND NOT M_BLOCK STREQUAL "none")
    set(_barrier_after ${M_BARRIER})
    set(_qbarrier ${M_BARRIER})
  else()
    message(FATAL_ERROR "kickos_add_driver(${name}): BARRIER is how many of its "
      "${_thread_count} threads are spawned before the readiness poll, whose latch is in the "
      "ring block, or none, not '${M_BARRIER}'")
  endif()

  set(_notify false)
  set(_notifications 0)
  if(M_NOTIFY)
    set(_notify true)
    set(_notifications ${KICKOS_DRIVER_NOTIFICATIONS})
  endif()
  set(_console false)
  if(M_CONSOLE)
    set(_console true)
  endif()
  set(_block 0)
  set(_qblock "\"none\"")
  if(NOT M_BLOCK STREQUAL "none")
    set(_block ${M_BLOCK})
    set(_qblock ${M_BLOCK})
  endif()
  # The libraries a task using the driver links, which kickos_compose links in a system naming it.
  set(_clients "")
  foreach(_c IN LISTS M_CLIENT)
    if(NOT _c MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
      message(FATAL_ERROR "kickos_add_driver(${name}): CLIENT names the library a client of the "
        "driver links, a target name, not '${_c}'")
    endif()
    _kickos_json_quote("${_c}" _qc)
    if(_qc IN_LIST _clients)
      message(FATAL_ERROR "kickos_add_driver(${name}): CLIENT names '${_c}' twice")
    endif()
    list(APPEND _clients "${_qc}")
  endforeach()
  set(_barrier true)
  if(M_BARRIER STREQUAL "none")
    set(_barrier false)
  endif()
  string(TOUPPER "${M_POSTURE}" _posture)
  list(LENGTH _windows _window_count)
  list(LENGTH _lines _line_count)

  _kickos_json_array(_jwindows ${_windows})
  _kickos_json_array(_jlines ${_lines})
  _kickos_json_array(_jthreads ${_threads})
  _kickos_json_array(_jclients ${_clients})
  _kickos_json_quote("${M_POSTURE}" _qposture)
  _kickos_json_quote("${M_BLOCK_CACHE}" _qblock_cache)
  set(_block_flags 0u)
  if(M_BLOCK_CACHE STREQUAL "uncached")
    set(_block_flags KOS_MEM_NOCACHE)
  endif()
  _kickos_json_quote("${M_START}" _qstart)
  _kickos_json_quote("${M_RECEIVER}" _qreceiver)
  set(${out_json} "{\"windows\": ${_jwindows}, \"lines\": ${_jlines}, \"threads\": ${_jthreads}, \
\"endpoints\": ${KICKOS_DRIVER_ENDPOINTS}, \"notifications\": ${_notifications}, \
\"block\": ${_qblock}, \"block_cache\": ${_qblock_cache}, \"posture\": ${_qposture}, \"barrier\": ${_qbarrier}, \"console\": ${_console}, \
\"start\": ${_qstart}, \"receiver\": ${_qreceiver}, \"client\": ${_jclients}}"
      PARENT_SCOPE)

  list(JOIN _prio ", " _prio_init)
  list(JOIN _caps ", " _caps_init)
  list(JOIN _badged ", " _badged_init)
  list(JOIN _names ", " _name_init)
  string(TOUPPER "${name}" _guard)
  set(${out_header}
"// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// GENERATED by kickos_add_driver(${name}) from its declaration; edits are overwritten by the
// next configure.

#ifndef KICKOS_DRIVER_DECLARED_${_guard}_H
#define KICKOS_DRIVER_DECLARED_${_guard}_H

#include <kickos/sys/driver_service.h>

extern \"C\" int ${M_START}(struct kos_service_cfg const* cfg);

namespace kickos::driver::declared::${name}
{
    constexpr ::kickos::driver::Declared k_declared = {
        .window_count = ${_window_count},
        .line_count = ${_line_count},
        .thread_count = ${_thread_count},
        .prio_delta = {${_prio_init}},
        .thread_name = {${_name_init}},
        .cap_count = {${_caps_init}},
        .badged = {${_badged_init}},
        .receiver = ${_receiver},
        .notify = ${_notify},
        .block_size = ${_block}u,
        .block_flags = ${_block_flags},
        .ep_posture = ::kickos::driver::KOS_DRV_EP_${_posture},
        .barrier = ${_barrier},
        .barrier_after = ${_barrier_after},
        .console = ${_console}
    };
}

#endif
" PARENT_SCOPE)
  set(${out_packaged} TRUE PARENT_SCOPE)
endfunction()

# Refuses packaged driver `name` when its catalogue entry `entry` names a CLIENT that no target
# KickOS::<client> is: kickos_compose links that target into every system naming the driver. Run
# once every target is declared, at the manifest's export.
function(kickos_driver_clients_exist name entry)
  string(JSON _count ERROR_VARIABLE _none LENGTH "${entry}" client)
  if(_none OR _count EQUAL 0)
    return()
  endif()
  math(EXPR _last "${_count} - 1")
  foreach(_i RANGE ${_last})
    string(JSON _client GET "${entry}" client ${_i})
    if(NOT TARGET KickOS::${_client})
      message(FATAL_ERROR "kickos_add_driver(${name}): CLIENT names '${_client}', and no target "
        "KickOS::${_client} exists once the build is declared; kickos_compose links it into every "
        "system naming the driver")
    endif()
  endforeach()
endfunction()
