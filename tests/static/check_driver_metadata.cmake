# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# kickos_driver_metadata's declarations and refusals, run under cmake -P:
#   cmake -DKICKOS_SOURCE_DIR=<repo root> -P tests/static/check_driver_metadata.cmake
# Each refusal is a child run of this file with CASE set, which must fail naming its rule.

cmake_minimum_required(VERSION 3.24)
include("${KICKOS_SOURCE_DIR}/cmake/driver_geometry.cmake")
include("${KICKOS_SOURCE_DIR}/cmake/driver_metadata.cmake")

set(_uart BASE 0x4000u WINDOWS no LINES n:level false:edge ignore:edge BLOCK 1024 INIT block_init BARRIER 1
          READY 4u POSTURE handover CONSOLE START uart_console_start CLIENT uart_proxy uart_stats
          THREAD off PRIORITY 1 ENTRY irq_entry ARG block WINDOW no CAPS notify:wait n:wait false:wait ignore:wait
          THREAD service ENTRY console_thread ARG block CAPS ep:wait notify:signal:doorbell)
set(_tail BLOCK none POSTURE retain BARRIER none START d_start)

if(DEFINED CASE)
  set(_one THREAD service ENTRY s CAPS ep:wait)
  if(CASE STREQUAL "bare_thread")
    kickos_driver_metadata(d _p _j _h SOURCES d.cc ${_tail} THREAD)
  elseif(CASE STREQUAL "metadata_alone")
    kickos_driver_metadata(d _p _j _h SOURCES d.cc WINDOWS regs)
  elseif(CASE STREQUAL "option_alone")
    kickos_driver_metadata(d _p _j _h CONSOLE)
  elseif(CASE STREQUAL "no_start")
    kickos_driver_metadata(d _p _j _h BLOCK none POSTURE retain BARRIER none ${_one})
  elseif(CASE STREQUAL "no_entry")
    kickos_driver_metadata(d _p _j _h ${_tail} THREAD service CAPS ep:wait)
  elseif(CASE STREQUAL "no_receiver")
    kickos_driver_metadata(d _p _j _h ${_tail} THREAD service ENTRY s)
  elseif(CASE STREQUAL "two_receivers")
    kickos_driver_metadata(d _p _j _h ${_tail} ${_one} THREAD worker ENTRY w CAPS ep:wait)
  elseif(CASE STREQUAL "unknown_argument")
    kickos_driver_metadata(d _p _j _h ${_tail} STACK 2048 ${_one})
  elseif(CASE STREQUAL "unknown_thread_argument")
    kickos_driver_metadata(d _p _j _h ${_tail} THREAD service ENTRY s STACK 2048 CAPS ep:wait)
  elseif(CASE STREQUAL "no_block")
    kickos_driver_metadata(d _p _j _h POSTURE retain BARRIER none START d_start ${_one})
  elseif(CASE STREQUAL "no_posture")
    kickos_driver_metadata(d _p _j _h BLOCK none BARRIER none START d_start ${_one})
  elseif(CASE STREQUAL "no_barrier")
    kickos_driver_metadata(d _p _j _h BLOCK none POSTURE retain START d_start ${_one})
  elseif(CASE STREQUAL "barrier_no_ready")
    kickos_driver_metadata(d _p _j _h BLOCK 1024 POSTURE retain BARRIER 1 START d_start ${_one})
  elseif(CASE STREQUAL "ready_no_barrier")
    kickos_driver_metadata(d _p _j _h ${_tail} READY 0u ${_one})
  elseif(CASE STREQUAL "window_unheld")
    kickos_driver_metadata(d _p _j _h ${_tail} WINDOWS regs ${_one})
  elseif(CASE STREQUAL "window_twice")
    kickos_driver_metadata(d _p _j _h ${_tail} WINDOWS regs ${_one} WINDOW regs THREAD irq ENTRY i WINDOW regs)
  elseif(CASE STREQUAL "window_undeclared")
    kickos_driver_metadata(d _p _j _h ${_tail} ${_one} WINDOW regs)
  elseif(CASE STREQUAL "line_no_trigger")
    kickos_driver_metadata(d _p _j _h ${_tail} LINES irq ${_one})
  elseif(CASE STREQUAL "grant_no_rights")
    kickos_driver_metadata(d _p _j _h ${_tail} THREAD service ENTRY s CAPS ep)
  elseif(CASE STREQUAL "grant_undeclared")
    kickos_driver_metadata(d _p _j _h ${_tail} ${_one} irq:wait)
  elseif(CASE STREQUAL "no_geometry")
    unset(KICKOS_DRIVER_ENDPOINTS)
    kickos_driver_metadata(d _p _j _h ${_tail} ${_one})
  elseif(CASE STREQUAL "client_empty")
    kickos_driver_metadata(d _p _j _h ${_tail} CLIENT "" ${_one})
  elseif(CASE STREQUAL "client_alone")
    kickos_driver_metadata(d _p _j _h SOURCES d.cc CLIENT d_proxy)
  elseif(CASE STREQUAL "client_no_target")
    kickos_driver_metadata(d _p _j _h ${_tail} CLIENT d_proxy ${_one})
    kickos_driver_clients_exist(d "${_j}")
  endif()
  return()
endif()

kickos_driver_metadata(plain _packaged _json _header SOURCES plain.cc REGDIR arch)
if(_packaged)
  message(FATAL_ERROR "FAIL: a driver with no THREAD was taken for a packaged one")
endif()

kickos_driver_metadata(uart _packaged _json _header SOURCES uart.cc ${_uart})
if(NOT _packaged)
  message(FATAL_ERROR "FAIL: a driver declaring a THREAD was not taken for a packaged one")
endif()
string(JSON _type ERROR_VARIABLE _bad TYPE "${_json}")
if(_bad)
  message(FATAL_ERROR "FAIL: the catalogue entry is not JSON: ${_bad}\n${_json}")
endif()
foreach(_check "windows;0;no" "lines;0;n" "lines;1;false" "lines;2;ignore" "threads;0;name;off"
               "threads;1;name;service" "threads;0;caps;4" "threads;0;badged;0" "threads;1;caps;2"
               "threads;1;badged;1" "threads;0;priority;1" "notifications;1" "start;uart_console_start"
               "client;0;uart_proxy" "client;1;uart_stats" "receiver;service" "block_cache;cached")
  list(POP_BACK _check _want)
  string(JSON _got GET "${_json}" ${_check})
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "FAIL: ${_check} is '${_got}', not the declared '${_want}'")
  endif()
endforeach()
foreach(_want ".tag = \"[uart] \"" ".expected_base = 0x4000u" ".block_size = 1024u" ".block_flags = 0u"
              ".ready_offset = 4u" ".barrier_after = 1" ".line_count = 3"
              ".lines = {{KOS_IRQ_LEVEL}, {KOS_IRQ_EDGE}, {KOS_IRQ_EDGE}}" ".entry = irq_entry"
              ".name = \"off\"" ".name = nullptr" ".window_grant = true" ".cap_count = 4"
              "{::kickos::driver::KOS_DRV_RES_LINE2, KOS_CAP_WAIT, 0}"
              "{::kickos::driver::KOS_DRV_RES_NOTIFY, KOS_CAP_SIGNAL, ::kickos::driver::doorbell_badge(3)}"
              ".arg = ::kickos::driver::KOS_DRV_ARG_BLOCK" ".ep_posture = ::kickos::driver::KOS_DRV_EP_HANDOVER"
              ".block_init = block_init" "#define KICKOS_DRIVER_BLOCK_SIZE 1024u"
              "extern \"C\" int uart_console_start(struct kos_driver_instance* instance);")
  string(FIND "${_header}" "${_want}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "FAIL: the generated header lacks `${_want}`:\n${_header}")
  endif()
endforeach()

kickos_driver_metadata(nocache _p _nocache_json _nocache_header TAG nc BLOCK 4096 BLOCK_CACHE uncached
                       POSTURE retain BARRIER 1 READY 0u START nocache_start THREAD service ENTRY s CAPS ep:wait)
string(JSON _got GET "${_nocache_json}" block_cache)
string(FIND "${_nocache_header}" ".block_flags = KOS_MEM_NOCACHE" _at)
string(FIND "${_nocache_header}" ".tag = \"[nc] \"" _tag_at)
if(NOT _got STREQUAL "uncached" OR _at EQUAL -1 OR _tag_at EQUAL -1)
  message(FATAL_ERROR "FAIL: BLOCK_CACHE uncached or TAG reached neither the entry ('${_got}') nor the header")
endif()

# No CLIENT: nothing to link, so nothing to find. No block and no barrier: no latch is polled.
kickos_driver_metadata(clientless _p _clientless_json _clientless_header ${_tail}
                       THREAD entry ENTRY e THREAD service ENTRY s CAPS ep:wait)
kickos_driver_clients_exist(clientless "${_clientless_json}")
foreach(_want ".ready_offset = ::kickos::driver::KOS_DRV_READY_NONE" ".barrier_after = 2" ".lines = {}"
              ".caps = {}" ".block_init = nullptr" ".expected_base = 0,")
  string(FIND "${_clientless_header}" "${_want}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "FAIL: the generated header lacks `${_want}`:\n${_clientless_header}")
  endif()
endforeach()
string(JSON _got GET "${_clientless_json}" notifications)
if(NOT _got EQUAL 0)
  message(FATAL_ERROR "FAIL: a driver granting no notify declares ${_got} notifications")
endif()

kickos_driver_metadata(cased _cased_packaged _cased_json _cased_header ${_tail}
                       CLIENT Uart_proxy _uart_proxy THREAD service ENTRY s CAPS ep:wait)
foreach(_check "client;0;Uart_proxy" "client;1;_uart_proxy")
  list(POP_BACK _check _want)
  string(JSON _got GET "${_cased_json}" ${_check})
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "FAIL: ${_check} is '${_got}', not the declared '${_want}'")
  endif()
endforeach()

foreach(_case_rule "bare_thread;THREAD given with no role" "metadata_alone;beside its THREADs"
                   "option_alone;beside its THREADs" "no_start;declares START"
                   "no_entry;THREAD service is not <role>" "no_receiver;0 threads are granted"
                   "two_receivers;2 threads are granted" "unknown_argument;unknown arguments STACK"
                   "unknown_thread_argument;THREAD service is not <role>" "no_block;declares BLOCK"
                   "no_posture;declares POSTURE" "no_barrier;declares BARRIER"
                   "barrier_no_ready;BARRIER 1 with READY" "ready_no_barrier;BARRIER none with READY"
                   "window_unheld;is held by 0"
                   "window_twice;is held by 2"
                   "window_undeclared;holds window 'regs'" "line_no_trigger;LINES entry 'irq'"
                   "grant_no_rights;grant 'ep' is not" "grant_undeclared;is granted 'irq'"
                   "no_geometry;no KICKOS_DRIVER_ENDPOINTS" "client_alone;beside its THREADs"
                   "client_empty;CLIENT given with no value"
                   "client_no_target;kickos_add_driver(d): CLIENT names 'd_proxy', and no target KickOS::d_proxy")
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
