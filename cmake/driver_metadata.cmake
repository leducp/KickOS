# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A packaged driver's declaration as kickos_add_driver takes it, rendered into the catalogue entry
# the manifest carries, whose rules the host tool checks at configure, and the header that holds
# its descriptor. Script-safe: it creates no target, so tests/static/check_driver_metadata.cmake
# drives it under cmake -P.

set(KICKOS_DRIVER_OPTIONS CONSOLE USB_DEVICE LOCAL)
set(KICKOS_DRIVER_SINGLE CLASS REGDIR TAG BASE BLOCK BLOCK_CACHE INIT POSTURE BARRIER READY START SLOTS)
set(KICKOS_DRIVER_MULTI SOURCES WINDOWS LINES CLIENT THREAD AUTHORITY)
set(KICKOS_DRIVER_METADATA TAG BASE WINDOWS LINES BLOCK BLOCK_CACHE INIT POSTURE BARRIER READY START CONSOLE
    USB_DEVICE CLIENT SLOTS AUTHORITY)
# The authority a driver's entry thread may run START with (<kickos/sys/cap_rights.h>).
set(KICKOS_DRIVER_AUTHORITIES memory pinmux pstate irq system console tasks bus_master bootloader)

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

# A count as a JSON number, anything else as a JSON string.
function(_kickos_json_count value out)
  if(value MATCHES "^(0|[1-9][0-9]*)$")
    set(${out} "${value}" PARENT_SCOPE)
  else()
    _kickos_json_quote("${value}" _q)
    set(${out} "${_q}" PARENT_SCOPE)
  endif()
endfunction()

# The JSON array of `items`, each already a JSON value.
function(_kickos_json_array out)
  list(JOIN ARGN ", " _body)
  set(${out} "[${_body}]" PARENT_SCOPE)
endfunction()

# kickos_driver_metadata(<name> <out_packaged> <out_json> <out_header> <kickos_add_driver args>...)
#   <out_packaged> is TRUE when the arguments declare a packaged driver, which a THREAD does; the
#   other two are then its catalogue entry as JSON and its <kickos/driver/declared/<name>.h>.
#   Every value is tested by DEFINED or by string, never by if(<value>): a role may be named
#   `off` or `no`, which CMake reads as false.
function(kickos_driver_metadata name out_packaged out_json out_header)
  # A THREAD runs to the next THREAD or to the next driver keyword.
  set(_keywords ${KICKOS_DRIVER_OPTIONS} ${KICKOS_DRIVER_SINGLE} ${KICKOS_DRIVER_MULTI})
  set(_args "")
  set(_thread_count 0)
  set(_in_thread FALSE)
  foreach(_arg IN LISTS ARGN)
    if(_arg STREQUAL "THREAD")
      math(EXPR _thread_count "${_thread_count} + 1")
      set(_thread_${_thread_count} "")
      set(_in_thread TRUE)
    elseif(_arg IN_LIST _keywords)
      set(_in_thread FALSE)
      list(APPEND _args "${_arg}")
    elseif(_in_thread)
      list(APPEND _thread_${_thread_count} "${_arg}")
    else()
      list(APPEND _args "${_arg}")
    endif()
  endforeach()
  cmake_parse_arguments(M "${KICKOS_DRIVER_OPTIONS}" "${KICKOS_DRIVER_SINGLE}" "${KICKOS_DRIVER_MULTI}" ${_args})
  if(DEFINED M_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "kickos_add_driver(${name}): unknown arguments ${M_UNPARSED_ARGUMENTS}")
  endif()
  if(DEFINED M_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "kickos_add_driver(${name}): ${M_KEYWORDS_MISSING_VALUES} given with no value")
  endif()
  set(${out_packaged} FALSE PARENT_SCOPE)
  if(_thread_count EQUAL 0)
    foreach(_key IN LISTS KICKOS_DRIVER_METADATA)
      if(DEFINED M_${_key} AND NOT "${M_${_key}}" STREQUAL "FALSE")
        message(FATAL_ERROR "kickos_add_driver(${name}): ${_key} is packaged-driver metadata, "
          "and a driver declares it beside its THREADs or not at all")
      endif()
    endforeach()
    return()
  endif()
  foreach(_key BLOCK POSTURE BARRIER START)
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
  if(NOT DEFINED M_BLOCK_CACHE)
    set(M_BLOCK_CACHE cached)
  endif()
  if(NOT DEFINED M_TAG)
    set(M_TAG "${name}")
  endif()
  if(NOT DEFINED M_BASE)
    set(M_BASE 0)
  endif()
  # The device slots the service keys on each client's badge, a slot mask of 32 bits.
  if(NOT DEFINED M_SLOTS)
    set(M_SLOTS 0)
  elseif(NOT M_SLOTS MATCHES "^([1-9]|[12][0-9]|3[0-2])$")
    message(FATAL_ERROR "kickos_add_driver(${name}): SLOTS ${M_SLOTS} is not a count from 1 to 32, "
      "the bits of the badge a client's slot mask rides")
  endif()
  set(_barrier_count FALSE)
  if(M_BARRIER MATCHES "^[0-9]+$")
    set(_barrier_count TRUE)
  endif()
  if((_barrier_count AND NOT DEFINED M_READY) OR (NOT _barrier_count AND DEFINED M_READY))
    message(FATAL_ERROR "kickos_add_driver(${name}): BARRIER ${M_BARRIER} with READY '${M_READY}'; "
      "a driver polls the READY latch after BARRIER threads, or names neither")
  endif()

  set(_line_roles "")
  set(_line_init "")
  set(_line_triggers "")
  foreach(_l IN LISTS M_LINES)
    if(NOT _l MATCHES "^([^:]+):(edge|level)$")
      message(FATAL_ERROR "kickos_add_driver(${name}): LINES entry '${_l}' is not <role>:edge or <role>:level")
    endif()
    list(APPEND _line_roles "${CMAKE_MATCH_1}")
    _kickos_json_quote("${CMAKE_MATCH_2}" _qtrigger)
    list(APPEND _line_triggers "${_qtrigger}")
    string(TOUPPER "${CMAKE_MATCH_2}" _trigger)
    list(APPEND _line_init "{KOS_IRQ_${_trigger}}")
  endforeach()
  list(LENGTH _line_roles _line_count)
  foreach(_w IN LISTS M_WINDOWS)
    set(_held_${_w} 0)
  endforeach()

  # A role is its thread's name, but the role `service`, whose thread takes its task's name.
  set(_drv "::kickos::driver::")
  set(_threads "")
  set(_thread_init "")
  set(_receivers "")
  set(_notify FALSE)
  # The entry thread's layout (<kickos/sys/driver_service.h>, layout_of): the first THREAD's
  # grants in their order, then every other token the task holds in token order.
  set(_entry "")
  set(_unbadged FALSE)
  set(_doorbell_held FALSE)
  foreach(_i RANGE 1 ${_thread_count})
    set(_t ${_thread_${_i}})
    list(LENGTH _t _n)
    if(_n EQUAL 0)
      message(FATAL_ERROR "kickos_add_driver(${name}): THREAD given with no role")
    endif()
    list(POP_FRONT _t _role)
    cmake_parse_arguments(T "" "PRIORITY;ENTRY;ARG;WINDOW" "CAPS" ${_t})
    if(DEFINED T_UNPARSED_ARGUMENTS OR DEFINED T_KEYWORDS_MISSING_VALUES OR NOT DEFINED T_ENTRY)
      message(FATAL_ERROR "kickos_add_driver(${name}): THREAD ${_role} is not <role> [PRIORITY <offset>] "
        "ENTRY <function> [ARG none|block|window|line0_index] [WINDOW <role>] [CAPS <grant>...]")
    endif()
    if(NOT DEFINED T_PRIORITY)
      set(T_PRIORITY 0)
    endif()
    if(NOT DEFINED T_ARG)
      set(T_ARG none)
    endif()
    set(_window false)
    if(DEFINED T_WINDOW)
      if(NOT T_WINDOW IN_LIST M_WINDOWS)
        message(FATAL_ERROR "kickos_add_driver(${name}): THREAD ${_role} holds window '${T_WINDOW}', "
          "which WINDOWS does not declare")
      endif()
      if(NOT _i EQUAL 1)
        message(FATAL_ERROR "kickos_add_driver(${name}): THREAD ${_role} holds window '${T_WINDOW}'; "
          "a window is seated on the task's entry thread, the first THREAD, and cannot move")
      endif()
      math(EXPR _held_${T_WINDOW} "${_held_${T_WINDOW}} + 1")
      set(_window true)
    endif()
    set(_caps "")
    set(_badged 0)
    foreach(_g IN LISTS T_CAPS)
      if(NOT _g MATCHES "^([^:]+):(wait|signal)(:doorbell)?$")
        message(FATAL_ERROR "kickos_add_driver(${name}): THREAD ${_role} grant '${_g}' is not "
          "<ep|notify|line role>:<wait|signal>, or notify:signal:doorbell")
      endif()
      set(_res "${CMAKE_MATCH_1}")
      set(_doorbell "${CMAKE_MATCH_3}")
      string(TOUPPER "${CMAKE_MATCH_2}" _rights)
      set(_badge 0)
      if(_res STREQUAL "ep")
        set(_resource "${_drv}KOS_DRV_RES_EP")
        set(_token "ep")
        if(_rights STREQUAL "WAIT")
          list(APPEND _receivers "${_role}")
        endif()
      elseif(_res STREQUAL "notify")
        set(_resource "${_drv}KOS_DRV_RES_NOTIFY")
        set(_notify TRUE)
        set(_token "notify")
        if(NOT _doorbell STREQUAL "")
          set(_token "doorbell")
          set(_doorbell_held TRUE)
        else()
          set(_unbadged TRUE)
        endif()
      else()
        list(FIND _line_roles "${_res}" _k)
        if(_k EQUAL -1)
          message(FATAL_ERROR "kickos_add_driver(${name}): THREAD ${_role} is granted '${_res}', which is "
            "neither ep, notify nor a role LINES declares")
        endif()
        set(_resource "${_drv}KOS_DRV_RES_LINE${_k}")
        set(_token "line:${_k}")
      endif()
      if(NOT _doorbell STREQUAL "")
        set(_badge "${_drv}doorbell_badge(${_line_count})")
        math(EXPR _badged "${_badged} + 1")
      endif()
      if(_i EQUAL 1)
        if("${_token}" IN_LIST _entry)
          message(FATAL_ERROR "kickos_add_driver(${name}): THREAD ${_role} is granted ${_token} twice; "
            "the entry thread holds each of its grants in one place")
        endif()
        list(APPEND _entry "${_token}")
      endif()
      list(APPEND _caps "{${_resource}, KOS_CAP_${_rights}, ${_badge}}")
    endforeach()
    list(LENGTH _caps _cap_count)
    list(JOIN _caps ", " _caps)
    set(_thread_name "nullptr")
    if(NOT _role STREQUAL "service")
      set(_thread_name "\"${_role}\"")
    endif()
    string(TOUPPER "${T_ARG}" _arg)
    string(APPEND _thread_init "            {.entry = ${T_ENTRY}, \\
             .name = ${_thread_name}, \\
             .prio_delta = ${T_PRIORITY}, \\
             .arg = ${_drv}KOS_DRV_ARG_${_arg}, \\
             .window_grant = ${_window}, \\
             .cap_count = ${_cap_count}, \\
             .caps = {${_caps}}}, \\
")
    _kickos_json_quote("${_role}" _qrole)
    _kickos_json_count("${T_PRIORITY}" _qprio)
    list(APPEND _threads "{\"name\": ${_qrole}, \"priority\": ${_qprio}, \"stack\": \"default\", \
\"caps\": ${_cap_count}, \"badged\": ${_badged}}")
  endforeach()
  set(_rest ep)
  if(_unbadged)
    list(APPEND _rest notify)
  endif()
  if(_doorbell_held)
    list(APPEND _rest doorbell)
  endif()
  if(_line_count GREATER 0)
    math(EXPR _last_line "${_line_count} - 1")
    foreach(_k RANGE ${_last_line})
      list(APPEND _rest "line:${_k}")
    endforeach()
  endif()
  foreach(_token IN LISTS _rest)
    if(NOT "${_token}" IN_LIST _entry)
      list(APPEND _entry "${_token}")
    endif()
  endforeach()
  set(_jentry "")
  foreach(_token IN LISTS _entry)
    _kickos_json_quote("${_token}" _qtoken)
    list(APPEND _jentry "${_qtoken}")
  endforeach()
  list(LENGTH _receivers _receiver_count)
  if(NOT _receiver_count EQUAL 1)
    message(FATAL_ERROR "kickos_add_driver(${name}): ${_receiver_count} threads are granted ep:wait "
      "(${_receivers}); exactly one receives on the endpoint")
  endif()
  foreach(_w IN LISTS M_WINDOWS)
    if(NOT _held_${_w} EQUAL 1)
      message(FATAL_ERROR "kickos_add_driver(${name}): window '${_w}' is held by ${_held_${_w}} threads; "
        "a window has one holder")
    endif()
  endforeach()

  set(_windows "")
  foreach(_r IN LISTS M_WINDOWS)
    _kickos_json_quote("${_r}" _q)
    list(APPEND _windows "${_q}")
  endforeach()
  set(_lines "")
  foreach(_r IN LISTS _line_roles)
    _kickos_json_quote("${_r}" _q)
    list(APPEND _lines "${_q}")
  endforeach()
  # A value that is no count reaches the catalogue as a string, which the host tool refuses.
  _kickos_json_count("${M_BLOCK}" _qblock)
  _kickos_json_count("${M_BARRIER}" _qbarrier)
  set(_block 0)
  if(M_BLOCK MATCHES "^[0-9]+$")
    set(_block ${M_BLOCK})
  endif()
  set(_barrier_after ${M_BARRIER})
  set(_ready "${M_READY}")
  if(NOT _barrier_count)
    set(_barrier_after ${_thread_count})
    set(_ready "${_drv}KOS_DRV_READY_NONE")
  elseif(M_BARRIER LESS 1 OR NOT M_BARRIER LESS _thread_count)
    message(FATAL_ERROR "kickos_add_driver(${name}): BARRIER ${M_BARRIER} with ${_thread_count} THREADs; "
      "the threads before it set the READY latch and the ones from it on wait for it, so it is "
      "at least 1 and below the THREAD count")
  endif()
  set(_notifications 0)
  if(_notify)
    set(_notifications ${KICKOS_DRIVER_NOTIFICATIONS})
  endif()
  set(_console false)
  if(M_CONSOLE)
    set(_console true)
  endif()
  set(_usb_device false)
  if(M_USB_DEVICE)
    set(_usb_device true)
  endif()
  set(_authority "")
  foreach(_a IN LISTS M_AUTHORITY)
    if(NOT _a IN_LIST KICKOS_DRIVER_AUTHORITIES)
      message(FATAL_ERROR "kickos_add_driver(${name}): AUTHORITY '${_a}' is none of ${KICKOS_DRIVER_AUTHORITIES}")
    endif()
    _kickos_json_quote("${_a}" _qa)
    list(APPEND _authority "${_qa}")
  endforeach()
  # The libraries a task using the driver links, which kickos_compose links in a system naming it.
  set(_clients "")
  foreach(_c IN LISTS M_CLIENT)
    _kickos_json_quote("${_c}" _qc)
    list(APPEND _clients "${_qc}")
  endforeach()
  string(TOUPPER "${M_POSTURE}" _posture)

  _kickos_json_array(_jwindows ${_windows})
  _kickos_json_array(_jlines ${_lines})
  _kickos_json_array(_jtriggers ${_line_triggers})
  _kickos_json_array(_jentry ${_jentry})
  _kickos_json_array(_jthreads ${_threads})
  _kickos_json_array(_jclients ${_clients})
  _kickos_json_array(_jauthority ${_authority})
  _kickos_json_quote("${M_POSTURE}" _qposture)
  _kickos_json_quote("${M_BLOCK_CACHE}" _qblock_cache)
  set(_block_flags 0u)
  if(M_BLOCK_CACHE STREQUAL "uncached")
    set(_block_flags KOS_MEM_NOCACHE)
  endif()
  _kickos_json_quote("${M_START}" _qstart)
  _kickos_json_quote("${_receivers}" _qreceiver)
  set(${out_json} "{\"windows\": ${_jwindows}, \"lines\": ${_jlines}, \"triggers\": ${_jtriggers}, \
\"entry\": ${_jentry}, \"threads\": ${_jthreads}, \
\"endpoints\": ${KICKOS_DRIVER_ENDPOINTS}, \"notifications\": ${_notifications}, \
\"block\": ${_qblock}, \"block_cache\": ${_qblock_cache}, \"posture\": ${_qposture}, \"barrier\": ${_qbarrier}, \"console\": ${_console}, \
\"usb_device\": ${_usb_device}, \"start\": ${_qstart}, \"receiver\": ${_qreceiver}, \"client\": ${_jclients}, \
\"slots\": ${M_SLOTS}, \"authority\": ${_jauthority}}"
      PARENT_SCOPE)

  set(_block_macro "")
  if(M_BLOCK MATCHES "^[0-9]+$")
    set(_block_macro "#define KICKOS_DRIVER_BLOCK_SIZE ${M_BLOCK}u\n\n")
  endif()
  if(NOT M_SLOTS EQUAL 0)
    string(APPEND _block_macro "#define KICKOS_DRIVER_SLOTS ${M_SLOTS}u\n\n")
  endif()
  set(_init nullptr)
  if(DEFINED M_INIT)
    set(_init "${M_INIT}")
  endif()
  list(JOIN _line_init ", " _line_init)
  string(TOUPPER "${name}" _guard)
  string(MAKE_C_IDENTIFIER "${name}" _cname)
  set(_launch nullptr)
  set(_launch_store "")
  if(_barrier_count)
    set(_launch "kickos_driver_launch_${_cname}")
    set(_launch_store "// What each thread past the barrier starts with, which every thread of the task reads.
[[maybe_unused]] static ${_drv}Launch ${_launch}[${_drv}KOS_DRV_THREADS_MAX];

")
  endif()
  set(${out_header}
"// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// GENERATED by kickos_add_driver(${name}) from its declaration; edits are overwritten by the
// next configure. KICKOS_DRIVER_DESCRIPTOR names the entries, the block initialiser and the
// expressions the declaration gives, so it is expanded where they are in scope.

#ifndef KICKOS_DRIVER_DECLARED_${_guard}_H
#define KICKOS_DRIVER_DECLARED_${_guard}_H

#include <kickos/sys/driver_service.h>

extern \"C\" int ${M_START}(struct kos_driver_instance* instance);

${_block_macro}${_launch_store}#define KICKOS_DRIVER_DESCRIPTOR \\
    { \\
        .tag = \"[${M_TAG}] \", \\
        .expected_base = ${M_BASE}, \\
        .block_size = ${_block}u, \\
        .block_flags = ${_block_flags}, \\
        .ready_offset = ${_ready}, \\
        .ep_posture = ${_drv}KOS_DRV_EP_${_posture}, \\
        .line_count = ${_line_count}, \\
        .thread_count = ${_thread_count}, \\
        .barrier_after = ${_barrier_after}, \\
        .lines = {${_line_init}}, \\
        .threads = { \\
${_thread_init}        }, \\
        .block_init = ${_init}, \\
        .launch = ${_launch} \\
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
