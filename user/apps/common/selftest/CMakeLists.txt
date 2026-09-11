# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A list: on the split parts these have to reach EVERY target, or the halves get different
# suites.
set(_selftest_defs "")

# The two `return <int>;` literals cmake/boot_arena.cmake scraped out of the backend .cc this
# link resolves. Unset on the sim, which reaches no scrape. Both derefs MUST stay quoted:
# get_property leaves the variable UNSET for a property never set, and an unquoted unset name
# compares as its own literal, so the guard would pass and define both macros EMPTY, which
# still satisfies `#if defined`.
get_property(_mpu_min_region_cfg GLOBAL PROPERTY KICKOS_MPU_MIN_REGION_CFG)
get_property(_mpu_region_pow2_cfg GLOBAL PROPERTY KICKOS_MPU_REGION_POW2_CFG)
if(NOT "${_mpu_min_region_cfg}" STREQUAL "" AND NOT "${_mpu_region_pow2_cfg}" STREQUAL "")
  list(APPEND _selftest_defs
    KICKOS_MPU_MIN_REGION_CFG=${_mpu_min_region_cfg}
    KICKOS_MPU_REGION_POW2_CFG=${_mpu_region_pow2_cfg})
endif()

# microbit's arena holds either the UART service's 1 KiB ring or the arena probes, so the choice
# is pinned by name here.
if(KICKOS_BOARD STREQUAL "microbit")
  list(APPEND _selftest_defs KICKOS_SELFTEST_NO_UART_SERVICE=1)
endif()

# The two 64 KiB FLASH parts (stm32f103 bluepill-c8, stm32f302), microbit (nrf51) and the
# ENFORCING esp32c6 variants build the suite as THREE images: main.cc partitions its
# registration list into three contiguous regions and KICKOS_SELFTEST_PART selects one. Each
# part plans 1..N from its OWN registry, so the bench has to run all three to cover the suite.
# Undefined elsewhere, where main.cc defaults it to 0 and registers everything.
#
# esp32c6's reason is a THIRD one, neither FLASH nor arena: its unprivileged code+rodata is ONE
# pow2 PMP NAPOT window (esp32c6.ld `_code_size`, 128 KiB), so the whole suite in one image
# overflows a window whose next step is 256 KiB and costs 128 KiB of thread arena. The flat
# variant carves no such window, so it stays a single image, which is why this clause reads
# KICKOS_HAVE_MPU and the other three read the chip alone.
set(_selftest_split 0)
if(KICKOS_CHIP STREQUAL "stm32f103" OR KICKOS_CHIP STREQUAL "stm32f302"
   OR KICKOS_CHIP STREQUAL "nrf51"
   OR (KICKOS_CHIP STREQUAL "esp32c6" AND KICKOS_HAVE_MPU))
  set(_selftest_split 1)
endif()

set(_selftest_srcs main.cc selftest_common.cc
                   "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc")
# Three blocks of arms sit in files of their own. Each still carries the `#if` its block had in
# main.cc, so a file compiled where its guard is off is empty and costs only the header; the
# conditions here spare that parse and must therefore MATCH those guards. Getting one wrong
# cannot shrink a suite: the registration list names every arm, so a file this list omits while
# main.cc still registers its arms is an undefined reference at link.
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST)
  list(APPEND _selftest_srcs selftest_aspace.cc)
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE)
  list(APPEND _selftest_srcs selftest_amp.cc)
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1)
  list(APPEND _selftest_srcs selftest_smp.cc)
endif()
# spi_mock.cc is a stand-in SPI backend under t_bus_device_slots, gated exactly like that arm. It
# DEFINES the public kos_spi_* names while the image may also carry a chip driver, which is
# legal only because the SPI SERVICE drivers rename their class symbols;
# tests/static/check_class_backend.sh asserts it per link.
if(KICKOS_ENABLE_SELFTEST)
  list(APPEND _selftest_srcs spi_mock.cc)
endif()

set(_selftest_images selftest)
if(_selftest_split)
  list(APPEND _selftest_images selftest_p2 selftest_p3)
endif()
foreach(_img IN LISTS _selftest_images)
  kickos_add_app_target(${_img} ${_selftest_srcs})
  target_link_libraries(${_img} PRIVATE kickos)
  kickos_emit_image(${_img})
  # PEAK concurrently held capabilities, not a sum over the run. The peak is main's 2 permanent
  # caps (g_lock, g_done) plus the widest mandatory arm's 3; the optional 3 more are
  # t_mutex_deadlock's, which SKIPS when it cannot allocate them.
  kickos_declare_app_capabilities(${_img} 5 3 ${KICKOS_CAP_REPLY_DEFAULT})
  target_include_directories(${_img} PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")
  target_compile_definitions(${_img} PRIVATE ${_selftest_defs})
  # At the fleet Debug default this app's .text overflows the 64K-flash parts.
  if(NOT KICKOS_ARCH STREQUAL "sim")
    target_compile_options(${_img} PRIVATE -Os)
  endif()
endforeach()
if(_selftest_split)
  target_compile_definitions(selftest    PRIVATE KICKOS_SELFTEST_PART=1)
  target_compile_definitions(selftest_p2 PRIVATE KICKOS_SELFTEST_PART=2)
  target_compile_definitions(selftest_p3 PRIVATE KICKOS_SELFTEST_PART=3)
endif()

# The linked image and its map, for the host gates that read them. On x86_64 the app target is
# an OBJECT library and the image is a PE32+ UEFI application a custom command writes, so
# $<TARGET_FILE:> names nothing and the -Map rides that ld line instead
# (cmake/x86_64_boot.cmake). Only the map names the archive MEMBER each symbol was resolved
# from, which is what proves a backend's definition beat its fallback.
#
# Recorded ON THE TARGET: the gates that read the map live on the test side, and a path
# restated there would be a second truth that drifts from the -Map this link actually passed.
if(KICKOS_ARCH STREQUAL "x86_64")
  get_target_property(_selftest_elf selftest KICKOS_IMAGE_FILE)
  set(_selftest_map "${_selftest_elf}.map")
else()
  set(_selftest_elf "$<TARGET_FILE:selftest>")
  set(_selftest_map "${_selftest_elf}.map")
  target_link_options(selftest PRIVATE "-Wl,-Map=${_selftest_map}")
  set_property(TARGET selftest APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${_selftest_map}")
endif()
set_target_properties(selftest PROPERTIES
  KICKOS_IMAGE_ELF "${_selftest_elf}" KICKOS_IMAGE_MAP "${_selftest_map}")

# An arm whose body left main.cc has EXTERNAL linkage, so the compiler no longer reports it
# unused when its TAP_ADD line goes. Every such arm is declared in selftest.h and registered in
# main.cc, and this holds the two sets equal, which is the guard -Werror=unused-function gives
# the arms that stayed. It reads the files rather than a list, so neither side can be satisfied
# by a name written here.
file(STRINGS main.cc _selftest_added REGEX "^ *TAP_ADD\\(")
set(_selftest_registered "")
foreach(_row IN LISTS _selftest_added)
  if(_row MATCHES "TAP_ADD\\(\"[^\"]*\", *([A-Za-z_][A-Za-z0-9_]*)\\)")
    list(APPEND _selftest_registered "${CMAKE_MATCH_1}")
  endif()
endforeach()
file(STRINGS selftest.h _selftest_declared_rows REGEX "void t_[A-Za-z0-9_]*\\(\\);")
foreach(_row IN LISTS _selftest_declared_rows)
  string(REGEX MATCH "(t_[A-Za-z0-9_]*)\\(\\);" _m "${_row}")
  if(NOT "${CMAKE_MATCH_1}" IN_LIST _selftest_registered)
    message(FATAL_ERROR
      "selftest.h declares the arm ${CMAKE_MATCH_1}, which no TAP_ADD line in main.cc "
      "registers. Its body is in a file of its own, so it has external linkage and the "
      "compiler cannot report it unused: the arm would be built into every image and run in "
      "none. Register it, or delete the declaration and the body together.")
  endif()
endforeach()
list(LENGTH _selftest_declared_rows _selftest_declared_n)
message(STATUS "selftest: ${_selftest_declared_n} arm(s) declared in selftest.h, all registered")

# The EXACT number of tap::add calls main.cc reaches in this posture. tap.cc plans `1..N` from the
# RUNTIME registry, so plan and case count shrink together when an arm is deleted and only an
# expectation from OUTSIDE the image can see it. The conditions mirror the #if guards around the
# registration block at the bottom of main.cc, one clause per guard.
set(_tap_arms 87) # unconditional, incl. ipc_one_buffer_both_ends, readable_global, endpoint_send_timeout, recv_timeout, timed_arg_refusals, 3 x call_timeout_*, reply_stale_caller, reply_abandoned_cap, thread_join, join_stale_gen, join_timeout, task_handles, task_member_refusals, task_creator_gate, task_group_kill, 3 x thread_slay_*, 2 x task_slay_*
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 14") # bus_device_slots, uart_service, reboot_priv, 11 x irq_*
  math(EXPR _tap_arms "${_tap_arms} + 1") # call_reg_fastpath (region 2)
  math(EXPR _tap_arms "${_tap_arms} + 4") # doorbell_xpoke, irq_kernel_line_reserved, prio_ceiling_refused, prio_ceiling_narrow_only
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 3") # endpoint_bound, grant_reserved, dev_window_exclusive
endif()
if(KICKOS_HAVE_MPU)
  math(EXPR _tap_arms "${_tap_arms} + 1") # stackbase_arena
endif()
# Both enforcing backends and neither flat one, which is the arm's own guard.
if(KICKOS_MEMORY_ENFORCED)
  math(EXPR _tap_arms "${_tap_arms} + 1") # cross_task_block
endif()
if(KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms "${_tap_arms} + 1") # caller_stack_arena
endif()
if(NOT KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms "${_tap_arms} + 1") # region_mode
endif()
if(KICKOS_ARCH STREQUAL "sim")
  math(EXPR _tap_arms "${_tap_arms} + 1") # periph_reg_write_mask
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 46") # frame_run_slot_recycle, call_reply_undisclosed, cap_objects, cap_map, recv_buf_unmapped, cap_map_over_stack, stack_grant_refused, stack_guard_intact, stack_handoff_refused, stack_slot_returns, cap_map_pins_run, cap_share, aspace_seam, aspace_model, aspace_map_cycle, aspace_translate, aspace_refusals, aspace_span, aspace_acquire_dup, aspace_balance, aspace_domain_balance, aspace_forced_unwind, aspace_churn, stack_is_frames, 2 x aspace_two_spaces_*, process_private_data, task_siblings_share, task_handoff_readback, task_handoff_donor_exits, task_handoff_slice, reservation_teardown, frame_scrub_cross_task, 2 x spawn_refusal_frees_*, parked_frame_hostile, split_access, process_ipc_same_addr, process_call_reply, grant_kernel_word_refused, self_grant_retype, reent_seating, aspace_acquire_balance, map_tlbi_elided, aspace_active_cores, process_data_template
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms "${_tap_arms} + 1") # fault_kills_task
endif()
# Mirrors the C guard, which is the AMP POSTURE and not the core count: an own-image node drives
# one core and registers these all the same.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE)
  # amp_far_call and amp_far_reply_guard are in this count on every posture: they skip by name
  # where no peer answers rather than being compiled out.
  math(EXPR _tap_arms "${_tap_arms} + 24") # amp_far_deliver_fault, amp_window, amp_reset_record, amp_port_seating, amp_port_unnamed, amp_far_call, amp_far_reply_guard, amp_far_reply_empty, amp_far_service, amp_far_refusal_answered, amp_deferred_doorbell, amp_app_alive, amp_inbound_reply, amp_far_undisclosed, amp_far_infoless, amp_reply_band, amp_mint_reply_port, amp_reply_reserve, amp_probe_root_only, amp_local_port_slot_held, amp_far_slot_reuse, amp_far_reset_answers, amp_far_answer_deferred, amp_far_tail_recovery
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _tap_arms "${_tap_arms} + 21") # pin_places, pin_wrong_core_never, unpin_restores, affinity_zero_defaults, affinity_undriven_refused, affinity_dead_handle_refused, migrate_running, pin_same_task_ok, pin_cross_task_refused, grant_narrows, grant_wider_refused, grant_after_member_refused, isolated_single_grant_ok, isolated_unpinned_never, isolated_unpin_excludes, isolated_takes_pinned, isolated_mixed_mask_ok, grant_second_narrow_only, grant_inherited_by_child_task, slice_preempts_every_core, threads_reach_every_core
endif()

# The same count per region. The regions are separated by the `#undef TAP_ADD` lines in main.cc:
# region 1 ends after call_timeout_reply, region 2 after cap_reply_slot_reuse, region 3 is the
# rest.
#
# These MUST stay independent expressions, never derived from _tap_arms above: the equality
# asserted below is only a check while both sides are stated separately, and a new arm has to
# move a clause on each side.
set(_tap_arms_p1 37) # region 1, unconditional, incl. endpoint_send_timeout, recv_timeout, timed_arg_refusals, 3 x call_timeout_*

set(_tap_arms_p2 23) # region 2, unconditional, incl. reply_stale_caller, reply_abandoned_cap, call_infoless_revert, call_close_reply, call_happy, call_from_root, call_truncation, call_double_reply, call_server_death, call_prepop_death, 4 x call_donation*, endpoint_crossdomain, 8 x cap_*
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p2 "${_tap_arms_p2} + 3") # call_reg_fastpath, bus_device_slots, uart_service
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p2 "${_tap_arms_p2} + 1") # endpoint_bound
endif()

set(_tap_arms_p3 27) # region 3, unconditional, incl. ipc_one_buffer_both_ends, console_publish_priv, shutdown_priv, writable_global, readable_global, authority_cap, periph_enable_unheld, periph_reg_write_unheld, privileged_spawn_refused, thread_join, join_stale_gen, join_timeout, task_handles, task_member_refusals, task_creator_gate, task_group_kill, 3 x thread_slay_*, 2 x task_slay_*, caller_stack, mem_self_grant_nonpow2, domain_share, mmio_grant, confused_deputy, mem_self_grant
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 12") # reboot_priv, 11 x irq_*
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 4") # doorbell_xpoke, irq_kernel_line_reserved, prio_ceiling_refused, prio_ceiling_narrow_only
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 2") # grant_reserved, dev_window_exclusive
endif()
if(KICKOS_HAVE_MPU)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # stackbase_arena
endif()
if(KICKOS_MEMORY_ENFORCED)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # cross_task_block
endif()
if(KICKOS_ARCH STREQUAL "sim")
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # periph_reg_write_mask
endif()
if(KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # caller_stack_arena
endif()
if(NOT KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # region_mode
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 46") # frame_run_slot_recycle, call_reply_undisclosed + cap_objects + cap_map + recv_buf_unmapped + cap_map_over_stack + 3 x stack_* refusals + stack_slot_returns + cap_map_pins_run + cap_share + 14 x aspace_* + stack_is_frames + process_private_data + process_data_template + task_siblings_share + task_handoff_readback + task_handoff_donor_exits + task_handoff_slice + self_grant_retype + reservation_teardown + frame_scrub_cross_task + 2 x spawn_refusal_frees_* + parked_frame_hostile + split_access + process_ipc_same_addr + process_call_reply + grant_kernel_word_refused + reent_seating + map_tlbi_elided + aspace_active_cores
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # fault_kills_task
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 24") # amp_far_deliver_fault, amp_window, amp_reset_record, amp_port_seating, amp_port_unnamed, amp_far_call, amp_far_reply_guard, amp_far_reply_empty, amp_far_service, amp_far_refusal_answered, amp_deferred_doorbell, amp_app_alive, amp_inbound_reply, amp_far_undisclosed, amp_far_infoless, amp_reply_band, amp_mint_reply_port, amp_reply_reserve, amp_probe_root_only, amp_local_port_slot_held, amp_far_slot_reuse, amp_far_reset_answers, amp_far_answer_deferred, amp_far_tail_recovery
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 21") # pin_places, pin_wrong_core_never, unpin_restores, affinity_zero_defaults, affinity_undriven_refused, affinity_dead_handle_refused, migrate_running, pin_same_task_ok, pin_cross_task_refused, grant_narrows, grant_wider_refused, grant_after_member_refused, isolated_single_grant_ok, isolated_unpinned_never, isolated_unpin_excludes, isolated_takes_pinned, isolated_mixed_mask_ok, grant_second_narrow_only, grant_inherited_by_child_task, slice_preempts_every_core, threads_reach_every_core
endif()

math(EXPR _tap_arms_split "${_tap_arms_p1} + ${_tap_arms_p2} + ${_tap_arms_p3}")
if(NOT _tap_arms_split EQUAL _tap_arms)
  message(FATAL_ERROR
    "selftest arm partition is not total: part 1 (${_tap_arms_p1}) + part 2 "
    "(${_tap_arms_p2}) + part 3 (${_tap_arms_p3}) = ${_tap_arms_split}, but the whole suite "
    "registers ${_tap_arms} "
    "arms in this posture. An arm was added, deleted or moved across the region boundary "
    "in main.cc without its per-part clause moving with it. Fix the counts in "
    "${CMAKE_CURRENT_LIST_FILE}, do not relax this check.")
endif()

if(_selftest_split)
  message(STATUS
    "selftest: split into three images: selftest plans ${_tap_arms_p1} arms, "
    "selftest_p2 plans ${_tap_arms_p2}, selftest_p3 plans ${_tap_arms_p3} "
    "(${_tap_arms} together)")
endif()

# tap.cc sizes its registry from KICKOS_TAP_MAX_TESTS. Per IMAGE and not per board, so it cannot
# go through _selftest_defs, which applies one list to both parts. An arm added without its
# clause overflows the registry and run_all() reports tap_registry_overflow.
#
# The SAME number is what a gate outside the image expects the run to plan, so each image
# records its own on itself and the gate reads it off the image it boots: a gate that picked
# p1/p2/p3 by hand would be a second statement of which part is which.
if(_selftest_split)
  set(_selftest_arms selftest ${_tap_arms_p1} selftest_p2 ${_tap_arms_p2}
                     selftest_p3 ${_tap_arms_p3})
else()
  set(_selftest_arms selftest ${_tap_arms})
endif()
while(_selftest_arms)
  list(POP_FRONT _selftest_arms _img _arms)
  target_compile_definitions(${_img} PRIVATE KICKOS_TAP_MAX_TESTS=${_arms})
  set_target_properties(${_img} PROPERTIES KICKOS_TAP_ARMS "${_arms}")
endwhile()
