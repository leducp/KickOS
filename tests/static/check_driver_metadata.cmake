# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# kickos_driver_metadata's declarations and refusals, run under cmake -P:
#   cmake -DKICKOS_SOURCE_DIR=<repo root> -P tests/static/check_driver_metadata.cmake
# Each refusal is a child run of this file with CASE set, which must fail naming its rule.

cmake_minimum_required(VERSION 3.24)
include("${KICKOS_SOURCE_DIR}/cmake/driver_geometry.cmake")
include("${KICKOS_SOURCE_DIR}/cmake/driver_metadata.cmake")

set(_uart THREADS off:1:default:2:0 service:0:default:2:1 RECEIVER service WINDOWS no LINES n false ignore NOTIFY
          BLOCK 1024 POSTURE handover BARRIER 1 START uart_console_start CONSOLE CLIENT uart_proxy uart_stats)

if(DEFINED CASE)
  if(CASE STREQUAL "bare_threads")
    kickos_driver_metadata(d _p _j _h SOURCES d.cc THREADS)
  elseif(CASE STREQUAL "metadata_alone")
    kickos_driver_metadata(d _p _j _h SOURCES d.cc WINDOWS regs)
  elseif(CASE STREQUAL "option_alone")
    kickos_driver_metadata(d _p _j _h CONSOLE)
  elseif(CASE STREQUAL "barrier_without_block")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none POSTURE retain BARRIER 1
                           START d_start)
  elseif(CASE STREQUAL "console_retained")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none POSTURE retain
                           BARRIER none START d_start CONSOLE)
  elseif(CASE STREQUAL "role_twice")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service LINES irq irq BLOCK none
                           POSTURE retain BARRIER none START d_start)
  elseif(CASE STREQUAL "no_start")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none POSTURE retain
                           BARRIER none)
  elseif(CASE STREQUAL "no_caps")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default RECEIVER service BLOCK none
                           POSTURE retain BARRIER none START d_start)
  elseif(CASE STREQUAL "no_receiver")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 BLOCK none POSTURE retain
                           BARRIER none START d_start)
  elseif(CASE STREQUAL "receiver_unknown")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER irq BLOCK none
                           POSTURE retain BARRIER none START d_start)
  elseif(CASE STREQUAL "start_not_symbol")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none POSTURE retain
                           BARRIER none START d-start)
  elseif(CASE STREQUAL "unknown_argument")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none POSTURE retain
                           BARRIER none START d_start STACK 2048)
  elseif(CASE STREQUAL "no_block")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service POSTURE retain
                           BARRIER none START d_start)
  elseif(CASE STREQUAL "no_posture")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none
                           BARRIER none START d_start)
  elseif(CASE STREQUAL "no_barrier")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none POSTURE retain
                           START d_start)
  elseif(CASE STREQUAL "no_geometry")
    unset(KICKOS_DRIVER_ENDPOINTS)
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none POSTURE retain
                           BARRIER none START d_start)
  elseif(CASE STREQUAL "block_not_number")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK 1k POSTURE retain
                           BARRIER none START d_start)
  elseif(CASE STREQUAL "block_not_pow2")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK 1000 POSTURE retain
                           BARRIER none START d_start)
  elseif(CASE STREQUAL "bad_posture")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none POSTURE publish
                           BARRIER none START d_start)
  elseif(CASE STREQUAL "offset_past_127")
    kickos_driver_metadata(d _p _j _h THREADS service:128:default:1:0 RECEIVER service BLOCK none POSTURE retain
                           BARRIER none START d_start)
  elseif(CASE STREQUAL "caps_past_255")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:256:0 RECEIVER service BLOCK none POSTURE retain
                           BARRIER none START d_start)
  elseif(CASE STREQUAL "badged_past_caps")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:2 RECEIVER service BLOCK none POSTURE retain
                           BARRIER none START d_start)
  elseif(CASE STREQUAL "barrier_past_threads")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK 1024 POSTURE retain
                           BARRIER 2 START d_start)
  elseif(CASE STREQUAL "thread_role_twice")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 service:1:default:1:0 RECEIVER service
                           BLOCK none POSTURE retain BARRIER none START d_start)
  elseif(CASE STREQUAL "client_not_target")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none
                           POSTURE retain BARRIER none START d_start CLIENT d-proxy)
  elseif(CASE STREQUAL "client_twice")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none
                           POSTURE retain BARRIER none START d_start CLIENT d_proxy d_proxy)
  elseif(CASE STREQUAL "client_empty")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none
                           POSTURE retain BARRIER none START d_start CLIENT "")
  elseif(CASE STREQUAL "client_alone")
    kickos_driver_metadata(d _p _j _h SOURCES d.cc CLIENT d_proxy)
  elseif(CASE STREQUAL "client_no_target")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none
                           POSTURE retain BARRIER none START d_start CLIENT d_proxy)
    kickos_driver_clients_exist(d "${_j}")
  elseif(CASE STREQUAL "bad_block_cache")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK 1024 BLOCK_CACHE nocache
                           POSTURE retain BARRIER 1 START d_start)
  elseif(CASE STREQUAL "uncached_no_block")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service BLOCK none BLOCK_CACHE uncached
                           POSTURE retain BARRIER none START d_start)
  elseif(CASE STREQUAL "role_not_name")
    kickos_driver_metadata(d _p _j _h THREADS service:0:default:1:0 RECEIVER service LINES Irq BLOCK none
                           POSTURE retain BARRIER none START d_start)
  endif()
  return()
endif()

kickos_driver_metadata(plain _packaged _json _header SOURCES plain.cc REGDIR arch)
if(_packaged)
  message(FATAL_ERROR "FAIL: a driver with no THREADS was taken for a packaged one")
endif()

kickos_driver_metadata(uart _packaged _json _header SOURCES uart.cc ${_uart})
if(NOT _packaged)
  message(FATAL_ERROR "FAIL: a driver declaring THREADS was not taken for a packaged one")
endif()
string(JSON _type ERROR_VARIABLE _bad TYPE "${_json}")
if(_bad)
  message(FATAL_ERROR "FAIL: the catalogue entry is not JSON: ${_bad}\n${_json}")
endif()
foreach(_check "windows;0;no" "lines;0;n" "lines;1;false" "lines;2;ignore" "threads;0;name;off"
               "threads;1;name;service" "threads;0;caps;2" "threads;0;badged;0" "threads;1;badged;1"
               "start;uart_console_start" "client;0;uart_proxy" "client;1;uart_stats"
               "receiver;service" "block_cache;cached")
  list(POP_BACK _check _want)
  string(JSON _got GET "${_json}" ${_check})
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "FAIL: ${_check} is '${_got}', not the declared '${_want}'")
  endif()
endforeach()
foreach(_want ".thread_name = {\"off\", nullptr}" ".line_count = 3" ".window_count = 1"
              ".notify = true" ".block_size = 1024u" ".block_flags = 0u" ".cap_count = {2, 2}" ".badged = {0, 1}"
              ".receiver = 1"
              "extern \"C\" int uart_console_start(struct kos_driver_instance* instance);")
  string(FIND "${_header}" "${_want}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "FAIL: the generated header lacks `${_want}`:\n${_header}")
  endif()
endforeach()

kickos_driver_metadata(nocache _p _nocache_json _nocache_header THREADS service:0:default:1:0 RECEIVER service
                       BLOCK 4096 BLOCK_CACHE uncached POSTURE retain BARRIER 1 START nocache_start)
string(JSON _got GET "${_nocache_json}" block_cache)
string(FIND "${_nocache_header}" ".block_flags = KOS_MEM_NOCACHE" _at)
if(NOT _got STREQUAL "uncached" OR _at EQUAL -1)
  message(FATAL_ERROR "FAIL: BLOCK_CACHE uncached reached neither the entry ('${_got}') nor the header")
endif()

# No CLIENT: nothing to link, so nothing to find.
kickos_driver_metadata(clientless _p _clientless_json _h THREADS service:0:default:1:0 RECEIVER service
                       BLOCK none POSTURE retain BARRIER none START clientless_start)
kickos_driver_clients_exist(clientless "${_clientless_json}")

kickos_driver_metadata(cased _cased_packaged _cased_json _cased_header THREADS service:0:default:1:0
                       RECEIVER service BLOCK none POSTURE retain BARRIER none START cased_start
                       CLIENT Uart_proxy _uart_proxy)
foreach(_check "client;0;Uart_proxy" "client;1;_uart_proxy")
  list(POP_BACK _check _want)
  string(JSON _got GET "${_cased_json}" ${_check})
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "FAIL: ${_check} is '${_got}', not the declared '${_want}'")
  endif()
endforeach()

foreach(_case_rule "bare_threads;given with no value" "metadata_alone;beside its THREADS"
                   "option_alone;beside its THREADS" "barrier_without_block;BARRIER"
                   "console_retained;POSTURE is handover" "role_twice;twice"
                   "no_start;declares START" "start_not_symbol;START is the C symbol"
                   "no_caps;:default:<capabilities" "no_receiver;declares RECEIVER"
                   "receiver_unknown;RECEIVER is the THREADS role" "unknown_argument;unknown arguments STACK"
                   "no_block;declares BLOCK" "no_posture;declares POSTURE" "no_barrier;declares BARRIER"
                   "no_geometry;no KICKOS_DRIVER_ENDPOINTS" "block_not_number;BLOCK is the ring block's size"
                   "block_not_pow2;BLOCK 1000 is no power of two" "bad_posture;POSTURE is handover or retain"
                   "offset_past_127;THREADS entry 'service:128:default:1:0'"
                   "caps_past_255;THREADS entry 'service:0:default:256:0'"
                   "badged_past_caps;THREADS entry 'service:0:default:1:2'"
                   "barrier_past_threads;BARRIER is how many of its 1 threads"
                   "thread_role_twice;THREADS names 'service' twice" "role_not_name;LINES role 'Irq'"
                   "client_not_target;CLIENT names the library" "client_twice;CLIENT names 'd_proxy' twice"
                   "client_alone;beside its THREADS" "client_empty;CLIENT given with no value"
                   "client_no_target;kickos_add_driver(d): CLIENT names 'd_proxy', and no target KickOS::d_proxy"
                   "bad_block_cache;BLOCK_CACHE is cached or uncached" "uncached_no_block;BLOCK_CACHE uncached types")
  list(GET _case_rule 0 _case)
  list(GET _case_rule 1 _rule)
  execute_process(COMMAND "${CMAKE_COMMAND}" -DKICKOS_SOURCE_DIR=${KICKOS_SOURCE_DIR}
                          -DCASE=${_case} -P "${CMAKE_CURRENT_LIST_FILE}"
                  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(_rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${_case} was declared without a refusal")
  endif()
  string(FIND "${_err}" "${_rule}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "FAIL: ${_case} was refused, but not for `${_rule}`:\n${_err}")
  endif()
endforeach()
message("PASS: driver metadata declarations and refusals")
