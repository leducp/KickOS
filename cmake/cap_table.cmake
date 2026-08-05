# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ROOT's capability-table width, summed at CONFIGURE time from four declarations,
# each made by whoever owns the fact (docs/design-capability-table.md section 6):
#
#   reserved indices      the kernel        KICKOS_CAP_FIRST_DYNAMIC (sys/cap_index.h)
#   retained for life     the service list  RETAINED_CAPS (kickos_add_board_provider)
#   peak concurrent       the app           CAPABILITIES (kickos_add_application)
#   peak inbound replies  whoever knows the protocol's fan-in, service list or app:
#                                           INBOUND_REPLY_CAPS (kickos_add_board_provider)
#                                           CAPABILITIES_INBOUND_REPLY (kickos_add_application)
#
# The summed width is what ROOT gets, and root alone. Every spawned child gets
# KICKOS_CAP_CHILD_WIDTH, the grant-list floor below, and nothing can ask for another width.
#
# The inbound-reply term is what the SERVER side of kos_call needs: cap_install_reply mints
# into the receiver's table and shares its one free list with the receiver's own creates
# (kernel/syscall/syscall_ipc.cc), so without a term for it the total is not a bound on when
# a task's own mint can fail. The widest declaration in the tree wins, like the app peak: it
# is a peak of CONCURRENTLY parked callers, never a count of calls over a run.
#
# The total is then RAISED to the grant-list floor, KICKOS_MAX_SPAWN_GRANTS + 1, whenever it
# falls below it: a full grant list lands at child indices 1..cap_count with no runtime check
# (cap.h), which is a property of the grant list and the reserved plane and not of anything
# any app holds. So the floor widens a narrow demand instead of refusing it -- an app is
# never asked to declare capabilities it does not hold.
#
# A board may state SUPPLY and nothing else (KICKOS_CAP_TABLE_SUPPLY, through the
# board_config.h #ifndef seam). A board header must NOT carry the width: it cannot know an
# app's working set or a chosen service list's retention, so it would go stale the moment
# either changes, with nothing to notice.
#
# Every input a header owns is read back through the real preprocessor with the same -D
# overrides the compile will see, as in cmake/boot_arena.cmake.

# What an app that declares nothing gets: the widest peak that still configures on the
# fleet's smallest supply (7 slots) once the reserved plane is paid, so a plain `int main`
# app links on every board. Raising it needs every board's supply raised with it.
set(KICKOS_CAP_APP_PEAK_DEFAULT 5)

# Inbound reply capabilities an undeclared task is assumed to hold. MUST stay 0: the three
# supply-7 boards (nrf51, stm32f103, stm32f302) sit at demand == supply already, so any
# nonzero fleet-wide value here stops them configuring. A task that really does hold
# concurrent parked callers declares them.
set(KICKOS_CAP_REPLY_DEFAULT 0)

# The provisioning integers as the compile will see them: the kernel's reserved range, the
# board's supply, the grant-list width, the thread count, the chunk granule and the count of
# runs held by something that is not a thread-pool slot.
function(kickos_cap_probe board_inc overrides out_reserved out_supply out_grants out_threads
                          out_chunk out_off_pool)
  set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${PROJECT_SOURCE_DIR}/kernel/include/kickos/config/system.h"
               "${PROJECT_SOURCE_DIR}/kernel/include/kickos/config/cap_geometry.h"
               "${PROJECT_SOURCE_DIR}/system/include/kickos/sys/cap_index.h"
               "${PROJECT_SOURCE_DIR}/kernel/include/kickos/cap.h")
  if(board_inc)
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${board_inc}/kickos/board_config.h")
  endif()
  set(_probe "${CMAKE_CURRENT_BINARY_DIR}/cap_table_probe.c")
  # config/system.h pulls board_config.h itself when the board ships one, so the sim (which
  # ships none) probes through the same file as every MCU.
  # ONLY input headers: cap.h reads the width this function is on its way to computing.
  file(WRITE "${_probe}"
    "#include <kickos/config/system.h>\n"
    "#include <kickos/config/cap_geometry.h>\n"
    "#include <kickos/sys/cap_index.h>\n"
    "#ifndef KCAP_CHUNK_TARGET\n"
    "#error \"KCAP_CHUNK_TARGET unset\"\n"
    "#endif\n"
    "#ifndef KICKOS_CAP_FIRST_DYNAMIC\n"
    "#error \"KICKOS_CAP_FIRST_DYNAMIC unset\"\n"
    "#endif\n"
    "#ifndef KICKOS_CAP_TABLE_SUPPLY\n"
    "#error \"KICKOS_CAP_TABLE_SUPPLY unset\"\n"
    "#endif\n"
    "#ifndef KICKOS_MAX_SPAWN_GRANTS\n"
    "#error \"KICKOS_MAX_SPAWN_GRANTS unset\"\n"
    "#endif\n"
    "#ifndef KICKOS_MAX_THREADS\n"
    "#error \"KICKOS_MAX_THREADS unset\"\n"
    "#endif\n"
    "kickos_cap_reserved KICKOS_CAP_FIRST_DYNAMIC\n"
    "kickos_cap_supply KICKOS_CAP_TABLE_SUPPLY\n"
    "kickos_cap_grants KICKOS_MAX_SPAWN_GRANTS\n"
    "kickos_cap_threads KICKOS_MAX_THREADS\n"
    "kickos_cap_chunk KCAP_CHUNK_TARGET\n")
  set(_flags "")
  foreach(_o IN LISTS overrides)
    list(APPEND _flags "-D${_o}")
  endforeach()
  set(_incs "-I${PROJECT_SOURCE_DIR}/kernel/include" "-I${PROJECT_SOURCE_DIR}/system/include"
            "-I${PROJECT_SOURCE_DIR}/include")
  if(board_inc)
    list(INSERT _incs 0 "-I${board_inc}")
  endif()
  execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -E -P -x c ${_incs} ${_flags} "${_probe}"
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
      "KickOS: could not read the capability-table provisioning through the preprocessor. "
      "Every board must leave KICKOS_CAP_TABLE_SUPPLY, KICKOS_MAX_SPAWN_GRANTS and "
      "KICKOS_MAX_THREADS defined (config/system.h carries the fleet defaults).\n${_err}")
  endif()
  foreach(_k reserved supply grants threads chunk)
    if(NOT "${_out}" MATCHES "kickos_cap_${_k}[ \t]+([^\r\n]+)")
      message(FATAL_ERROR "KickOS: capability probe produced no ${_k} value:\n${_out}")
    endif()
    math(EXPR _v_${_k} "${CMAKE_MATCH_1}")
  endforeach()

  # KCAP_RUN_OFF_POOL is a constexpr, not a macro, so the preprocessor cannot hand it over the
  # way it hands KCAP_CHUNK_TARGET: read its declaration. A rename or a change of type FATALs
  # here rather than leaving the .bss figures below on a literal of their own.
  set(_off_pool "")
  file(STRINGS "${PROJECT_SOURCE_DIR}/kernel/include/kickos/cap.h" _off_pool_lines
       REGEX "KCAP_RUN_OFF_POOL[ \t]*=")
  foreach(_l IN LISTS _off_pool_lines)
    # The trailing `;` is required: without it `= 1 + 1;` reads as 1, and a hex literal as 0.
    if(_l MATCHES "constexpr[ \t]+uint16_t[ \t]+KCAP_RUN_OFF_POOL[ \t]*=[ \t]*([0-9]+)[ \t]*;")
      set(_off_pool "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  if(_off_pool STREQUAL "")
    message(FATAL_ERROR
      "KickOS: kernel/include/kickos/cap.h no longer declares KCAP_RUN_OFF_POOL as a "
      "`constexpr uint16_t KCAP_RUN_OFF_POOL = <literal>`. The run count in the .bss figures "
      "below is read from that declaration so the two cannot drift; match the new form here.")
  endif()
  math(EXPR _v_off_pool "${_off_pool}")

  set(${out_reserved} "${_v_reserved}" PARENT_SCOPE)
  set(${out_supply} "${_v_supply}" PARENT_SCOPE)
  set(${out_grants} "${_v_grants}" PARENT_SCOPE)
  set(${out_threads} "${_v_threads}" PARENT_SCOPE)
  set(${out_chunk} "${_v_chunk}" PARENT_SCOPE)
  set(${out_off_pool} "${_v_off_pool}" PARENT_SCOPE)
endfunction()

# The chunk geometry cap.h will compile for `slots`. MIRRORS the #if in cap.h: one exact-width
# chunk when the whole table fits the granule, else a ceiling count of granule-wide chunks.
# Only the reported footprint is derived here; the kernel computes its own.
function(_kickos_cap_geometry slots chunk out_chunks out_reserved_slots)
  if(NOT slots GREATER chunk)
    set(${out_chunks} 1 PARENT_SCOPE)
    set(${out_reserved_slots} "${slots}" PARENT_SCOPE)
    return()
  endif()
  math(EXPR _n "(${slots} + ${chunk} - 1) / ${chunk}")
  math(EXPR _r "${_n} * ${chunk}")
  set(${out_chunks} "${_n}" PARENT_SCOPE)
  set(${out_reserved_slots} "${_r}" PARENT_SCOPE)
endfunction()

# The slab's chunk count and .bss: one child-width run per holder, plus root's own widening
# on top. Every run holder is GUARANTEED the child width and every spawn asks for exactly
# that, so a spawn can never be refused for want of a chunk.
# MIRRORS KCAP_SLAB_CHUNKS in kernel/include/kickos/cap.h; the two must move together.
function(_kickos_cap_slab slots child chunk threads off_pool
                          out_bytes out_chunks out_child_chunks)
  _kickos_cap_geometry("${slots}" "${chunk}" _root_chunks _root_res)
  math(EXPR _chunk_slots "${_root_res} / ${_root_chunks}")
  math(EXPR _child_chunks "(${child} + ${_chunk_slots} - 1) / ${_chunk_slots}")
  math(EXPR _n "(${threads} + ${off_pool}) * ${_child_chunks} + ${_root_chunks} - ${_child_chunks}")
  math(EXPR _b "${_n} * ${_chunk_slots} * 8")
  set(${out_bytes} "${_b}" PARENT_SCOPE)
  set(${out_chunks} "${_n}" PARENT_SCOPE)
  set(${out_child_chunks} "${_child_chunks}" PARENT_SCOPE)
endfunction()

# The width the package was BUILT with: the resolve's own answer in tree, else what
# KickOSConfig.cmake recorded for an installed package. "" when neither is set. Only the
# out-of-tree warning below may use it; what a COMPILE reads is the generated header.
function(_kickos_cap_installed_width out)
  set(${out} "" PARENT_SCOPE)
  get_property(_resolved_width GLOBAL PROPERTY KICKOS_CAP_TABLE_WIDTH)
  if("${_resolved_width}" MATCHES "^[0-9]+$")
    set(${out} "${_resolved_width}" PARENT_SCOPE)
    return()
  endif()
  if("${KICKOS_CAP_TABLE_WIDTH}" MATCHES "^[0-9]+$")
    set(${out} "${KICKOS_CAP_TABLE_WIDTH}" PARENT_SCOPE)
  endif()
endfunction()

# Record one app target's declared demand. `peak` is what must ALWAYS fit; `optional` is
# further demand whose arms reclaim and self-skip when they cannot allocate, so it is
# granted only when the board's supply covers it. `reply` is the peak of CONCURRENT INBOUND
# reply capabilities this app's tasks are the server side of. All three are a PEAK of
# CONCURRENTLY held capabilities, never a sum over the arms of a run.
function(kickos_declare_app_capabilities target peak optional reply)
  foreach(_n "${peak}" "${optional}" "${reply}")
    if(NOT "${_n}" MATCHES "^[0-9]+$")
      message(FATAL_ERROR "kickos_declare_app_capabilities(${target}): '${_n}' is not a "
        "non-negative integer count of concurrently held capabilities")
    endif()
  endforeach()
  set_target_properties(${target} PROPERTIES
    KICKOS_CAP_PEAK "${peak}" KICKOS_CAP_PEAK_OPTIONAL "${optional}"
    KICKOS_CAP_REPLY "${reply}")
  set_property(GLOBAL APPEND PROPERTY KICKOS_CAP_APP_TARGETS "${target}")
  # A declaration made after the sum is resolved cannot move it: the width is already compiled
  # into the libraries. Keyed on the resolve having RUN, not on being out of tree -- an
  # add_subdirectory(KickOS) or FetchContent consumer HAS boards/, so it reads as in-tree while
  # its declarations land after the root CMakeLists.txt has already resolved, which is the same
  # silent drop a find_package consumer gets. Every in-tree declaration site precedes the
  # resolve, so this cannot fire on them.
  get_property(_resolved GLOBAL PROPERTY KICKOS_CAP_TABLE_RESOLVED)
  if(_resolved OR NOT KICKOS_IN_TREE)
    # Two shapes reach here and the remedy differs, so do not call both an installed package: a
    # find_package consumer cannot re-sum at all, while an add_subdirectory or FetchContent
    # consumer can, by declaring before KickOS is added.
    _kickos_cap_installed_width(_installed)
    set(_width "already decided")
    if(NOT _installed STREQUAL "")
      set(_width "${_installed} slot(s)")
    endif()
    if(_resolved AND KICKOS_IN_TREE)
      set(_how "Declare it BEFORE add_subdirectory(KickOS), so the sum sees it.")
    else()
      string(CONCAT _how "An installed package cannot be re-summed: rebuild KickOS with this "
                         "demand declared in tree, on a board whose KICKOS_CAP_TABLE_SUPPLY "
                         "can back it.")
      if(_installed STREQUAL "")
        string(APPEND _how " The width is in <prefix>/include/kickos/config/cap_width.h "
                           "as KICKOS_MAX_HANDLES.")
      endif()
    endif()
    message(WARNING
      "kickos_declare_app_capabilities(${target}): this declaration (peak ${peak}, optional "
      "${optional}, inbound reply ${reply}) CANNOT be honoured here. The capability-table "
      "width is summed once, when KickOS itself is configured, and compiled into its "
      "libraries; that sum has already run, so root's width is ${_width} and this declaration "
      "reaches nothing. A create past it returns -KOS_EMFILE at runtime, whatever this says. "
      "${_how}")
  endif()
endfunction()

# Sum the declarations, check the total against the board's supply, and forward the width.
# Call ONCE, after every subdirectory that can declare (user/apps is added last).
function(kickos_cap_table_resolve board_inc overrides service_list out_slots out_chunk
                                  out_child_width out_reply_max)
  kickos_cap_probe("${board_inc}" "${overrides}" _reserved _supply _grants _threads _chunk
                   _off_pool)

  set(_retained 0)
  set(_retained_by "${service_list}")
  set(_reply 0)
  set(_reply_by "nothing in the tree")
  if(TARGET ${service_list})
    # Matched numerically, never by truthiness: an unset property reads as `<var>-NOTFOUND`,
    # and `if(-1)` is TRUE, so a negative would reach math(EXPR) below and silently NARROW the
    # sum. kickos_add_board_provider refuses a non-integer, so a non-match here means undeclared.
    get_target_property(_declared ${service_list} KICKOS_CAP_RETAINED)
    if(_declared MATCHES "^[0-9]+$")
      set(_retained "${_declared}")
    endif()
    get_target_property(_declared_reply ${service_list} KICKOS_CAP_REPLY)
    if(_declared_reply MATCHES "^[0-9]+$" AND _declared_reply GREATER _reply)
      set(_reply "${_declared_reply}")
      set(_reply_by "${service_list}")
    endif()
  else()
    set(_retained_by "${service_list} (not a target; nothing retained)")
  endif()

  # The widest declaration wins: one kernel archive serves every app in the tree, so the
  # table has to hold the most demanding one. Each maximum is tracked with the app that set
  # it, so the refusals below name a declaration somebody can go and change.
  set(_undeclared "the undeclared-app default")
  set(_peak "${KICKOS_CAP_APP_PEAK_DEFAULT}")
  set(_peak_by "${_undeclared}")
  set(_full "${KICKOS_CAP_APP_PEAK_DEFAULT}")
  set(_full_by "${_undeclared}")
  get_property(_apps GLOBAL PROPERTY KICKOS_CAP_APP_TARGETS)
  foreach(_a IN LISTS _apps)
    get_target_property(_p ${_a} KICKOS_CAP_PEAK)
    get_target_property(_o ${_a} KICKOS_CAP_PEAK_OPTIONAL)
    get_target_property(_r ${_a} KICKOS_CAP_REPLY)
    math(EXPR _pf "${_p} + ${_o}")
    if(_r GREATER _reply)
      set(_reply "${_r}")
      set(_reply_by "${_a}")
    endif()
    # A tie with the default names the app anyway: "the default" is not something a reader
    # can go and change.
    if(_p GREATER _peak)
      set(_peak "${_p}")
      set(_peak_by "${_a}")
    elseif(_p EQUAL _peak AND _peak_by STREQUAL _undeclared)
      set(_peak_by "${_a}")
    endif()
    if(_pf GREATER _full)
      set(_full "${_pf}")
      set(_full_by "${_a}")
    elseif(_pf EQUAL _full AND _full_by STREQUAL _undeclared)
      set(_full_by "${_a}")
    endif()
  endforeach()

  math(EXPR _sum "${_reserved} + ${_retained} + ${_peak} + ${_reply}")
  math(EXPR _sum_full "${_reserved} + ${_retained} + ${_full} + ${_reply}")

  # A table that cannot seat the reserved plane plus a full grant list is unsound whatever any
  # app declared: delegated cap i lands at child index i+1 and nothing checks it at runtime
  # (cap.h). The floor answers to KICKOS_MAX_SPAWN_GRANTS, not to anything an app holds, so it
  # RAISES a narrower demand. Refusing one instead would make every app in the fleet declare
  # up to the floor whether it holds that much or not.
  #
  # The reply term belongs in the floor and not only in the sum: the sum widens ROOT, while
  # KICKOS_CAP_REPLY_MAX bounds reply capabilities on every task alike, and every server in
  # the tree is a spawned child seated at this floor.
  math(EXPR _floor "${_grants} + 1 + ${_reply}")

  # Of cap.h's three bounds on the child width, this is the one that does not hold by
  # construction: a small enough KICKOS_MAX_SPAWN_GRANTS puts a child's WHOLE table inside
  # the reserved range.
  if(NOT _floor GREATER _reserved)
    message(FATAL_ERROR
      "KickOS: a spawned child's capability table would be ${_floor} slot(s), which is "
      "entirely inside the ${_reserved}-slot reserved index range (KICKOS_CAP_FIRST_DYNAMIC), "
      "so no child could ever create a capability of its own. Every spawned child is seated "
      "at this width and nothing can ask for another. It is KICKOS_MAX_SPAWN_GRANTS=${_grants} "
      "+ 1 + ${_reply} declared inbound reply cap(s); raise KICKOS_MAX_SPAWN_GRANTS.")
  endif()

  set(_req "${_sum}")
  set(_want "${_sum_full}")
  set(_floor_note "")
  if(_floor GREATER _req)
    set(_req "${_floor}")
    set(_floor_note " -- BINDING, wider than the demand above")
  endif()
  if(_floor GREATER _want)
    set(_want "${_floor}")
  endif()

  set(_terms
    "  reserved indices, kernel (KICKOS_CAP_FIRST_DYNAMIC) : ${_reserved}\n"
    "  retained for life by ${_retained_by} : ${_retained}\n"
    "  peak concurrent, declared by ${_peak_by} : ${_peak}\n"
    "  peak inbound reply caps, declared by ${_reply_by} : ${_reply}\n"
    "  = demand : ${_sum}\n"
    "  grant-list floor, KICKOS_MAX_SPAWN_GRANTS ${_grants} + 1 + ${_reply} reply : "
    "${_floor}${_floor_note}\n"
    "  board supply (KICKOS_CAP_TABLE_SUPPLY) : ${_supply}\n")

  if(_floor GREATER _supply)
    math(EXPR _short "${_floor} - ${_supply}")
    message(FATAL_ERROR
      "KickOS: a full spawn grant list cannot fit root's capability table on board "
      "'${KICKOS_BOARD}': KICKOS_MAX_SPAWN_GRANTS=${_grants} needs ${_floor} slot(s) (grant i "
      "lands at child index i+1, and nothing checks it at runtime), and the board supplies "
      "${_supply} -- short by ${_short}.\n" ${_terms}
      "The grant list and the reserved plane are most of this floor and no app declaration "
      "moves them; the inbound-reply term IS an app or service-list declaration, and it is "
      "${_reply} here. Lower KICKOS_MAX_SPAWN_GRANTS, lower the declared INBOUND_REPLY_CAPS / "
      "CAPABILITIES_INBOUND_REPLY, or raise the board's KICKOS_CAP_TABLE_SUPPLY if its RAM "
      "really can back it.")
  endif()

  # Supply is checked against DEMAND, not against the chunk-rounded reservation: rounding the
  # check up would newly refuse a board whose supply is not a multiple of the granule.
  _kickos_cap_geometry("${_req}" "${_chunk}" _req_chunks _req_res)
  _kickos_cap_slab("${_req}" "${_floor}" "${_chunk}" "${_threads}" "${_off_pool}"
                   _bytes _slab_chunks _child_chunks)
  if(_req GREATER _supply)
    math(EXPR _short "${_req} - ${_supply}")
    message(FATAL_ERROR
      "KickOS: root's capability table needs ${_req} slot(s), but board "
      "'${KICKOS_BOARD}' supplies ${_supply} -- short by ${_short} slot(s) "
      "(${_req_chunks} chunk(s) per widest run, ${_slab_chunks} chunk(s) of slab = "
      "${_bytes} B of Kernel .bss).\n"
      ${_terms}
      "The board states supply only. Lower a demand above, or raise the board's "
      "KICKOS_CAP_TABLE_SUPPLY if its RAM really can back it.")
  endif()

  set(_slots "${_req}")
  if(NOT _want GREATER _supply)
    set(_slots "${_want}")
  endif()
  _kickos_cap_geometry("${_slots}" "${_chunk}" _chunks _res_slots)
  _kickos_cap_slab("${_slots}" "${_floor}" "${_chunk}" "${_threads}" "${_off_pool}"
                   _bytes _slab_chunks _child_chunks)
  # string(CONCAT), never set() with several arguments: that makes a LIST, and a list deref
  # inside message() shows its separating semicolons.
  string(CONCAT _why
    "${_reserved} reserved + ${_retained} retained (${_retained_by}) + ${_peak} app peak "
    "(${_peak_by}) + ${_reply} inbound reply (${_reply_by})")
  if(_floor GREATER _sum)
    string(CONCAT _why
      "the KICKOS_MAX_SPAWN_GRANTS ${_grants} grant-list floor, wider than the demand "
      "${_sum} = ${_why}")
  endif()
  message(STATUS "KickOS: cap table = ${_slots} slot(s) = ${_why}; supply ${_supply}, "
                 "${_bytes} B .bss")
  # A run is reserved in whole chunks, so the last one's tail is paid for and unaddressable.
  if(_chunks EQUAL 1)
    message(STATUS "KickOS: cap table: 1 chunk of ${_res_slots} -- the flat run, no directory, "
                   "no shift, nothing rounded up")
  else()
    math(EXPR _tail "${_res_slots} - ${_slots}")
    message(STATUS "KickOS: cap table: ${_chunks} chunks of ${_chunk} = ${_res_slots} slot(s) "
                   "reserved per run, ${_tail} of them unaddressable tail")
  endif()
  if(_want GREATER _supply)
    math(EXPR _opt "${_full} - ${_peak}")
    message(STATUS "KickOS: cap table: ${_opt} optional slot(s) of ${_full_by} NOT granted "
                   "(${_want} > supply ${_supply}) -- an arm that needs them reclaims and "
                   "skips at runtime")
  elseif(_full GREATER _peak)
    math(EXPR _opt "${_full} - ${_peak}")
    message(STATUS "KickOS: cap table: ${_opt} optional slot(s) of ${_full_by} granted")
  endif()

  math(EXPR _root_extra "${_chunks} - ${_child_chunks}")
  message(STATUS "KickOS: cap table: child width ${_floor} slot(s) = ${_child_chunks} "
                 "chunk(s) guaranteed to each of ${_threads} + ${_off_pool} run holder(s), "
                 "plus ${_root_extra} chunk(s) for root's own widening")

  # Never 0: a task that may hold no inbound reply capability could never serve a kos_call.
  # It reserves no slot, so raising the floor to 1 costs no width.
  set(_reply_max "${_reply}")
  if(_reply_max LESS 1)
    set(_reply_max 1)
  endif()

  # A child must keep at least one dynamic slot once the reply bound is spent, or peers alone
  # could fill its table. Subsumes the reserved-plane refusal above numerically, but names the
  # declared reply peak rather than KICKOS_MAX_SPAWN_GRANTS.
  math(EXPR _child_own "${_floor} - ${_reserved} - ${_reply_max}")
  if(_child_own LESS 1)
    message(FATAL_ERROR
      "KickOS: a spawned child would keep ${_child_own} slot(s) of its own once inbound "
      "reply capabilities are accounted: child width ${_floor} - ${_reserved} reserved - "
      "${_reply_max} inbound reply. Peers could then fill its table on their own, which is "
      "what KICKOS_CAP_REPLY_MAX exists to prevent. Raise KICKOS_MAX_SPAWN_GRANTS, or lower "
      "the declared INBOUND_REPLY_CAPS / CAPABILITIES_INBOUND_REPLY.")
  endif()

  set_property(GLOBAL PROPERTY KICKOS_CAP_TABLE_RESOLVED TRUE)
  set_property(GLOBAL PROPERTY KICKOS_CAP_TABLE_WIDTH "${_slots}")
  set(${out_slots} "${_slots}" PARENT_SCOPE)
  set(${out_chunk} "${_chunk}" PARENT_SCOPE)
  set(${out_child_width} "${_floor}" PARENT_SCOPE)
  set(${out_reply_max} "${_reply_max}" PARENT_SCOPE)
endfunction()
