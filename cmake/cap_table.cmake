# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The per-task capability-table width, summed at CONFIGURE time from three declarations,
# each made by whoever owns the fact (docs/design-capability-table.md section 6):
#
#   reserved indices      the kernel        KICKOS_CAP_FIRST_DYNAMIC (sys/cap_index.h)
#   retained for life     the service list  RETAINED_CAPS (kickos_add_board_provider)
#   peak concurrent       the app           CAPABILITIES (kickos_add_application)
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

# The provisioning integers as the compile will see them: the kernel's reserved range, the
# board's supply, the grant-list width, the thread count and the chunk granule.
function(kickos_cap_probe board_inc overrides out_reserved out_supply out_grants out_threads
                          out_chunk)
  set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${PROJECT_SOURCE_DIR}/kernel/include/kickos/config/system.h"
               "${PROJECT_SOURCE_DIR}/system/include/kickos/sys/cap_index.h"
               "${PROJECT_SOURCE_DIR}/kernel/include/kickos/cap.h")
  if(board_inc)
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${board_inc}/kickos/board_config.h")
  endif()
  set(_probe "${CMAKE_CURRENT_BINARY_DIR}/cap_table_probe.c")
  # config/system.h pulls board_config.h itself when the board ships one, so the sim (which
  # ships none) probes through the same file as every MCU.
  # cap.h is preprocessed, never compiled, so its static_asserts (which need the summed width
  # this function is on its way to computing) are not evaluated.
  file(WRITE "${_probe}"
    "#include <kickos/config/system.h>\n"
    "#include <kickos/sys/cap_index.h>\n"
    "#include <kickos/cap.h>\n"
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
  set(${out_reserved} "${_v_reserved}" PARENT_SCOPE)
  set(${out_supply} "${_v_supply}" PARENT_SCOPE)
  set(${out_grants} "${_v_grants}" PARENT_SCOPE)
  set(${out_threads} "${_v_threads}" PARENT_SCOPE)
  set(${out_chunk} "${_v_chunk}" PARENT_SCOPE)
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

# Record one app target's declared demand. `peak` is what must ALWAYS fit; `optional` is
# further demand whose arms reclaim and self-skip when they cannot allocate, so it is
# granted only when the board's supply covers it. Both are a PEAK of CONCURRENTLY held
# capabilities, never a sum over the arms of a run.
function(kickos_declare_app_capabilities target peak optional)
  foreach(_n "${peak}" "${optional}")
    if(NOT "${_n}" MATCHES "^[0-9]+$")
      message(FATAL_ERROR "kickos_declare_app_capabilities(${target}): '${_n}' is not a "
        "non-negative integer count of concurrently held capabilities")
    endif()
  endforeach()
  set_target_properties(${target} PROPERTIES
    KICKOS_CAP_PEAK "${peak}" KICKOS_CAP_PEAK_OPTIONAL "${optional}")
  set_property(GLOBAL APPEND PROPERTY KICKOS_CAP_APP_TARGETS "${target}")
endfunction()

# Apply `def` to every target in the tree, including those already created. NOT
# add_compile_definitions(): a subdirectory COPIES COMPILE_DEFINITIONS when it is added, so a
# value known only after user/apps never reaches an earlier component. A directory's OWN
# property is read at generate time, so appending per directory does.
function(_kickos_cap_define_tree def)
  set(_dirs "${PROJECT_SOURCE_DIR}")
  while(_dirs)
    list(POP_FRONT _dirs _d)
    set_property(DIRECTORY "${_d}" APPEND PROPERTY COMPILE_DEFINITIONS "${def}")
    get_property(_subs DIRECTORY "${_d}" PROPERTY SUBDIRECTORIES)
    list(APPEND _dirs ${_subs})
  endwhile()
endfunction()

# Sum the declarations, check the total against the board's supply, and forward the width.
# Call ONCE, after every subdirectory that can declare (user/apps is added last).
function(kickos_cap_table_resolve board_inc overrides service_list out_slots)
  kickos_cap_probe("${board_inc}" "${overrides}" _reserved _supply _grants _threads _chunk)

  set(_retained 0)
  set(_retained_by "${service_list}")
  if(TARGET ${service_list})
    get_target_property(_declared ${service_list} KICKOS_CAP_RETAINED)
    if(_declared)
      set(_retained "${_declared}")
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
    math(EXPR _pf "${_p} + ${_o}")
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

  math(EXPR _req "${_reserved} + ${_retained} + ${_peak}")
  math(EXPR _want "${_reserved} + ${_retained} + ${_full}")
  set(_terms
    "  reserved indices, kernel (KICKOS_CAP_FIRST_DYNAMIC) : ${_reserved}\n"
    "  retained for life by ${_retained_by} : ${_retained}\n"
    "  peak concurrent, declared by ${_peak_by} : ${_peak}\n"
    "  = demand : ${_req}\n"
    "  board supply (KICKOS_CAP_TABLE_SUPPLY) : ${_supply}\n")

  # A table that cannot seat the reserved plane plus a full grant list is unsound whatever
  # any app asked for: delegated cap i lands at child index i+1 and nothing checks it at
  # runtime (cap.h).
  math(EXPR _floor "${_grants} + 1")
  if(_req LESS _floor)
    math(EXPR _short "${_floor} - ${_req}")
    message(FATAL_ERROR
      "KickOS: the per-task capability table sums to ${_req} slot(s), which cannot seat a "
      "full spawn grant list -- KICKOS_MAX_SPAWN_GRANTS=${_grants} needs ${_floor}, short "
      "by ${_short}.\n" ${_terms}
      "Raise the app's CAPABILITIES to what it really holds at once, or lower "
      "KICKOS_MAX_SPAWN_GRANTS.")
  endif()

  # Supply is checked against DEMAND, not against the chunk-rounded reservation: rounding the
  # check up would newly refuse a board whose supply is not a multiple of the granule.
  _kickos_cap_geometry("${_req}" "${_chunk}" _req_chunks _req_res)
  math(EXPR _bytes "${_req_res} * (${_threads} + 2) * 8")
  if(_req GREATER _supply)
    math(EXPR _short "${_req} - ${_supply}")
    message(FATAL_ERROR
      "KickOS: the per-task capability table needs ${_req} slot(s), but board "
      "'${KICKOS_BOARD}' supplies ${_supply} -- short by ${_short} slot(s) "
      "(${_req_chunks} chunk(s) = ${_req_res} slot(s) reserved x (KICKOS_MAX_THREADS "
      "${_threads} + 2) x 8 = ${_bytes} B of Kernel .bss).\n"
      ${_terms}
      "The board states supply only. Lower a demand above, or raise the board's "
      "KICKOS_CAP_TABLE_SUPPLY if its RAM really can back it.")
  endif()

  set(_slots "${_req}")
  if(NOT _want GREATER _supply)
    set(_slots "${_want}")
  endif()
  _kickos_cap_geometry("${_slots}" "${_chunk}" _chunks _res_slots)
  math(EXPR _bytes "${_res_slots} * (${_threads} + 2) * 8")
  message(STATUS "KickOS: cap table = ${_slots} slot(s) = ${_reserved} reserved + "
                 "${_retained} retained (${_retained_by}) + ${_peak} app peak "
                 "(${_peak_by}); supply ${_supply}, ${_bytes} B .bss")
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

  _kickos_cap_define_tree("KICKOS_MAX_HANDLES=${_slots}")
  set(${out_slots} "${_slots}" PARENT_SCOPE)
endfunction()
