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

# The two 64 KiB FLASH parts (stm32f103 bluepill-c8, stm32f302) and microbit (nrf51) build the
# suite as THREE images: main.cc partitions its registration list into three contiguous regions
# and KICKOS_SELFTEST_PART selects one. Each part plans 1..N from its OWN registry, so the
# bench has to run all three to cover the suite. Undefined elsewhere, where main.cc defaults it
# to 0 and registers everything.
set(_selftest_split 0)
if(KICKOS_CHIP STREQUAL "stm32f103" OR KICKOS_CHIP STREQUAL "stm32f302"
   OR KICKOS_CHIP STREQUAL "nrf51")
  set(_selftest_split 1)
endif()

# PEAK concurrently held capabilities, not a sum over the run. CAPABILITIES is main's 2 permanent
# caps (g_lock, g_done) plus the widest mandatory arm's 3; CAPABILITIES_OPTIONAL is 3 more for
# t_mutex_deadlock, which SKIPS when it cannot allocate them.
set(_selftest_srcs main.cc "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc")
# spi_mock.cc is a stand-in SPI backend under t_bus_device_slots, gated exactly like that arm. It
# DEFINES the public kos_spi_* names while the image may also carry a chip driver, which is
# legal only because the SPI SERVICE drivers rename their class symbols;
# tests/static/check_class_backend.sh asserts it per link.
if(KICKOS_ENABLE_SELFTEST)
  list(APPEND _selftest_srcs spi_mock.cc)
endif()
kickos_add_application(selftest SOURCES ${_selftest_srcs} BOARD ${KICKOS_BOARD}
  CAPABILITIES 5 CAPABILITIES_OPTIONAL 3)
set(_selftest_images selftest)
if(_selftest_split)
  kickos_add_application(selftest_p2 SOURCES ${_selftest_srcs} BOARD ${KICKOS_BOARD}
    CAPABILITIES 5 CAPABILITIES_OPTIONAL 3)
  kickos_add_application(selftest_p3 SOURCES ${_selftest_srcs} BOARD ${KICKOS_BOARD}
    CAPABILITIES 5 CAPABILITIES_OPTIONAL 3)
  list(APPEND _selftest_images selftest_p2 selftest_p3)
  target_compile_definitions(selftest    PRIVATE KICKOS_SELFTEST_PART=1)
  target_compile_definitions(selftest_p2 PRIVATE KICKOS_SELFTEST_PART=2)
  target_compile_definitions(selftest_p3 PRIVATE KICKOS_SELFTEST_PART=3)
endif()

foreach(_img IN LISTS _selftest_images)
  target_include_directories(${_img} PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")
  target_compile_definitions(${_img} PRIVATE ${_selftest_defs})
  # At the fleet Debug default this app's .text overflows the 64K-flash parts.
  if(NOT (KICKOS_ARCH STREQUAL "sim"))
    target_compile_options(${_img} PRIVATE -Os)
  endif()
endforeach()

# The linked image and its map, for the two host gates below. On x86_64 the app target is an
# OBJECT library and the image is a PE32+ UEFI application a custom command writes, so
# $<TARGET_FILE:> names nothing and the -Map rides that ld line instead
# (cmake/x86_64_boot.cmake). Only the map names the archive MEMBER each symbol was resolved
# from, which is what proves a backend's definition beat its fallback.
if(KICKOS_ARCH STREQUAL "x86_64")
  get_target_property(_selftest_elf selftest KICKOS_IMAGE_FILE)
  set(_selftest_map "${_selftest_elf}.map")
else()
  set(_selftest_elf "$<TARGET_FILE:selftest>")
  set(_selftest_map "${_selftest_elf}.map")
  target_link_options(selftest PRIVATE "-Wl,-Map=${_selftest_map}")
  set_property(TARGET selftest APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${_selftest_map}")
endif()

# The expected-skip list every gate below carries; checked by name in
# tests/integration/check_tap_stream.sh.
set(KICKOS_EXPECT_SKIPS "")
# Every arm whose pass rests on two threads' relative progress, or on one thread being denied
# the CPU by another. The claim is a single-core one (main.cc, TAP_SKIP_ONE_CORE_ORDER) and the
# invariants behind these arms are still checked on every one-core preset in the fleet.
if(KICKOS_KERNEL_CORES GREATER 1)
  list(APPEND KICKOS_EXPECT_SKIPS
    fifo_order preempt_on_ready irq_thread_ctx sleep_order
    mutex_pi_donation mutex_chain_boost mutex_multi_held mutex_deadlock
    reply_abandoned_cap call_timeout_revert call_infoless_revert call_close_reply
    call_donation call_donation_hold call_donation_slow call_donation_pending
    cap_reply_bound_fast cap_reply_bound_slow thread_slay_timeout
    mutex_owner_died_nowaiter aspace_two_spaces_same_grant
    parked_frame_hostile)
endif()

# The isolated-core mask, normalised: the knob is hex text, which `if(... EQUAL 0)` does not
# read as a number.
set(_selftest_isolated 0)
if(DEFINED KICKOS_ISOLATED_CORES AND NOT KICKOS_ISOLATED_CORES STREQUAL "")
  math(EXPR _selftest_isolated "${KICKOS_ISOLATED_CORES} | 0")
endif()
# Five placement arms assert something about an ISOLATED core, and a posture that isolates
# none has no such core to assert it of. They run on qemu-arm64's smpiso variant.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1 AND _selftest_isolated EQUAL 0)
  list(APPEND KICKOS_EXPECT_SKIPS
    isolated_single_grant_ok isolated_unpinned_never isolated_unpin_excludes
    isolated_takes_pinned isolated_mixed_mask_ok)
endif()

# migrate_running moves a spinner off a core above the boot core and onto the boot core, so it
# counts the cores in [1, KICKOS_KERNEL_CORES) that the isolated mask does not name. DERIVED
# and not a posture list: the arm scans exactly this set and takes the first it finds. Core 0,
# the destination, is one no image may isolate.
set(_selftest_movable 0)
if(KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _selftest_top "${KICKOS_KERNEL_CORES} - 1")
  foreach(_c RANGE 1 ${_selftest_top})
    math(EXPR _selftest_c_iso "(${_selftest_isolated} >> ${_c}) & 1")
    if(_selftest_c_iso EQUAL 0)
      math(EXPR _selftest_movable "${_selftest_movable} + 1")
    endif()
  endforeach()
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1 AND _selftest_movable LESS 1)
  list(APPEND KICKOS_EXPECT_SKIPS migrate_running)
endif()

# slice_preempts_every_core and threads_reach_every_core each release one thread MORE than the
# machine has kernel cores, and both ask pool_can_host for exactly that number before spawning.
# DERIVED and not a posture list. KICKOS_MAX_THREADS is the whole bound visible here; slots a
# service-list driver holds are NOT, which is why the arms ask at runtime too.
set(_selftest_crowd 0)
if(KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _selftest_crowd "${KICKOS_KERNEL_CORES} + 1")
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1
   AND KICKOS_MAX_THREADS LESS ${_selftest_crowd})
  list(APPEND KICKOS_EXPECT_SKIPS slice_preempts_every_core threads_reach_every_core)
endif()

# The same for the arms that report PARTIAL. A PARTIAL reports `ok`, so neither the plan/case
# reconciliation nor the skip bookkeeping can see one and this by-name set is the only thing
# that can. It is NOT derivable from _tap_arms: the conditions below name the postures.
set(KICKOS_EXPECT_PARTIALS "")
# Above one kernel core, five arms can each reach a half they cannot judge, for two different
# reasons. FOUR tier-1 IRQ arms carry a claim about an event that must NOT happen (a service,
# a redelivery or a wake), and a non-event raises nothing to order a later read after, so
# there is no closed interval to read the result in. thread_slay_window is the other reason:
# its control leg needs the victim to reach a park before the kill lands, and that race is the
# machine's to grant. Each arm names its own in main.cc, and the one-core fleet checks them
# all. thread_slay_window partials only when its restage budget is spent, so the name is
# listed here rather than reported every run.
if(KICKOS_KERNEL_CORES GREATER 1)
  list(APPEND KICKOS_EXPECT_PARTIALS irq_spurious irq_mask_coalesce irq_discard
                                     irq_stale_register thread_slay_window)
endif()
if(KICKOS_ARCH STREQUAL "sim")
  list(APPEND KICKOS_EXPECT_PARTIALS periph_reg_write_unheld)
endif()
if(KICKOS_ARCH STREQUAL "armv8a")
  list(APPEND KICKOS_EXPECT_PARTIALS periph_reg_write_unheld)
endif()
if(KICKOS_ARCH STREQUAL "rv64imac")
  list(APPEND KICKOS_EXPECT_PARTIALS periph_reg_write_unheld)
endif()
if(KICKOS_ARCH STREQUAL "x86_64")
  list(APPEND KICKOS_EXPECT_PARTIALS periph_reg_write_unheld)
endif()
# cap_chunk_span needs a table WIDER than the chunk granule (KICKOS_CAP_CHUNK_TARGET) to reach a
# segmented index. The summed width is not readable here, cmake/cap_table.cmake resolving it
# after this directory is added, but the SUPPLY is. cap_child_width is on the same condition.
if(KICKOS_CAP_TABLE_SUPPLY LESS_EQUAL KICKOS_CAP_CHUNK_TARGET)
  list(APPEND KICKOS_EXPECT_PARTIALS cap_chunk_span cap_child_width)
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  # The reserved-overlap matrix needs at least one arch_reserved_blocks entry, which the
  # qemu-riscv PMP port has and the host and mps2 parts do not.
  if(NOT KICKOS_BOARD STREQUAL "qemu-riscv")
    list(APPEND KICKOS_EXPECT_PARTIALS grant_reserved)
  endif()
  if(KICKOS_ARCH STREQUAL "sim")
    list(APPEND KICKOS_EXPECT_PARTIALS dev_window_exclusive)
  endif()
endif()
# The threads permitted to fault, by NAME. Every other thread-fault record in the stream is an
# arm dying the wrong way, and a contained fault reconciles the plan and the case count, so this
# set is the only thing that can tell the two apart (tests/integration/check_tap_stream.sh).
set(KICKOS_EXPECT_FAULTS "")
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION)
  list(APPEND KICKOS_EXPECT_FAULTS fvic)
endif()

# Comma-separated, never semicolons: ENVIRONMENT is itself a CMake list, so a raw list
# deref would split the value into further bogus environment entries.
list(JOIN KICKOS_EXPECT_SKIPS "," KICKOS_EXPECT_SKIPS)
list(JOIN KICKOS_EXPECT_PARTIALS "," KICKOS_EXPECT_PARTIALS)
list(JOIN KICKOS_EXPECT_FAULTS "," KICKOS_EXPECT_FAULTS)

# The EXACT number of tap::add calls main.cc reaches in this posture. tap.cc plans `1..N` from the
# RUNTIME registry, so plan and case count shrink together when an arm is deleted and only an
# expectation from OUTSIDE the image can see it. The conditions mirror the #if guards around the
# registration block at the bottom of main.cc, one clause per guard.
set(_tap_arms 86) # unconditional, incl. readable_global, endpoint_send_timeout, recv_timeout, timed_arg_refusals, 3 x call_timeout_*, reply_stale_caller, reply_abandoned_cap, thread_join, join_stale_gen, join_timeout, task_handles, task_member_refusals, task_creator_gate, task_group_kill, 3 x thread_slay_*, 2 x task_slay_*
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 14") # bus_device_slots, uart_service, reboot_priv, 11 x irq_*
  math(EXPR _tap_arms "${_tap_arms} + 1") # call_reg_fastpath (region 2)
  math(EXPR _tap_arms "${_tap_arms} + 2") # prio_ceiling_refused, prio_ceiling_narrow_only
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 3") # endpoint_bound, grant_reserved, dev_window_exclusive
endif()
if(KICKOS_HAVE_MPU)
  math(EXPR _tap_arms "${_tap_arms} + 1") # stackbase_arena
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
  math(EXPR _tap_arms "${_tap_arms} + 45") # cap_objects, cap_map, cap_map_over_stack, stack_grant_refused, stack_guard_intact, stack_handoff_refused, stack_slot_returns, cap_map_pins_run, cap_share, aspace_seam, aspace_model, aspace_map_cycle, aspace_translate, aspace_refusals, aspace_span, aspace_acquire_dup, aspace_balance, aspace_domain_balance, aspace_forced_unwind, aspace_churn, stack_is_frames, 2 x aspace_two_spaces_*, process_private_data, task_siblings_share, task_handoff_readback, task_handoff_donor_exits, task_handoff_slice, self_grant_cross_task, reservation_teardown, frame_scrub_cross_task, 2 x spawn_refusal_frees_*, parked_frame_hostile, split_access, process_ipc_same_addr, process_call_reply, grant_kernel_word_refused, self_grant_retype, reent_seating, aspace_acquire_balance, map_tlbi_elided, aspace_active_cores, doorbell_xpoke, process_data_template
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms "${_tap_arms} + 1") # fault_kills_task
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_NUM_CORES GREATER 1
   AND KICKOS_MULTICORE_MODEL_SHARED EQUAL 0)
  math(EXPR _tap_arms "${_tap_arms} + 5") # amp_window, amp_far_call, amp_far_reply_guard, amp_probe_root_only, amp_far_slot_reuse
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

set(_tap_arms_p3 26) # region 3, unconditional, incl. console_publish_priv, shutdown_priv, writable_global, readable_global, authority_cap, periph_enable_unheld, periph_reg_write_unheld, privileged_spawn_refused, thread_join, join_stale_gen, join_timeout, task_handles, task_member_refusals, task_creator_gate, task_group_kill, 3 x thread_slay_*, 2 x task_slay_*, caller_stack, mem_self_grant_nonpow2, domain_share, mmio_grant, confused_deputy, mem_self_grant
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 12") # reboot_priv, 11 x irq_*
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 2") # prio_ceiling_refused, prio_ceiling_narrow_only
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 2") # grant_reserved, dev_window_exclusive
endif()
if(KICKOS_HAVE_MPU)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # stackbase_arena
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
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 45") # cap_objects + cap_map + cap_map_over_stack + 3 x stack_* refusals + stack_slot_returns + cap_map_pins_run + cap_share + 14 x aspace_* + stack_is_frames + process_private_data + process_data_template + task_siblings_share + task_handoff_readback + task_handoff_donor_exits + task_handoff_slice + self_grant_cross_task + self_grant_retype + reservation_teardown + frame_scrub_cross_task + 2 x spawn_refusal_frees_* + parked_frame_hostile + split_access + process_ipc_same_addr + process_call_reply + grant_kernel_word_refused + reent_seating + map_tlbi_elided + aspace_active_cores + doorbell_xpoke
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # fault_kills_task
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_NUM_CORES GREATER 1
   AND KICKOS_MULTICORE_MODEL_SHARED EQUAL 0)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 5") # amp_window, amp_far_call, amp_far_reply_guard, amp_probe_root_only, amp_far_slot_reuse
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

# tap.cc sizes its registry from this. Per IMAGE and not per board, so it cannot go through
# _selftest_defs, which applies one list to both parts. An arm added without its clause overflows
# the registry and run_all() reports tap_registry_overflow.
if(_selftest_split)
  target_compile_definitions(selftest    PRIVATE KICKOS_TAP_MAX_TESTS=${_tap_arms_p1})
  target_compile_definitions(selftest_p2 PRIVATE KICKOS_TAP_MAX_TESTS=${_tap_arms_p2})
  target_compile_definitions(selftest_p3 PRIVATE KICKOS_TAP_MAX_TESTS=${_tap_arms_p3})
else()
  target_compile_definitions(selftest PRIVATE KICKOS_TAP_MAX_TESTS=${_tap_arms})
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME selftest
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_selftest.sh" "$<TARGET_FILE:selftest>"
            ${_tap_arms})
  set_tests_properties(selftest PROPERTIES TIMEOUT 30)
  set_property(TEST selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")

  # The sim's only coverage of the published console route. One provider links per image, so the
  # script makes its own build tree, and every input the arm count depends on has to travel with
  # it: a fresh build otherwise takes the board's base variant and every knob default.
  add_test(
    NAME    sim_published_console
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_published.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" ${_tap_arms}
            "${KICKOS_CONFIG_VARIANT}")
  set_tests_properties(sim_published_console PROPERTIES TIMEOUT 300)
  set_property(TEST sim_published_console APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")

  add_test(
    NAME    oot_export
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_oot_export.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  set_tests_properties(oot_export PROPERTIES TIMEOUT 120)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_selftest TARGET selftest BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms})
  set_property(TEST ${KICKOS_QEMU_MPS2_TAG}_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
endif()
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  # The sets below are partitioned BY REGION, NOT BY NAME: an arm belongs to the part whose
  # region holds its TAP_ADD line in main.cc. check_tap_stream.sh reports a name declared in
  # the wrong part as a NOTE and not a failure, so only that rule catches one.
  kickos_add_qemu_test(NAME microbit_selftest TARGET selftest BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms_p1})
  set_property(TEST microbit_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS="
    "EXPECT_PARTIALS="
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
  # Both lists are a MEASUREMENT and not slack: a listed arm that did NOT skip is only a NOTE, so
  # a stale list quietly permits a regression.
  kickos_add_qemu_test(NAME microbit_selftest_p2 TARGET selftest_p2 BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms_p2})
  set_property(TEST microbit_selftest_p2 APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=uart_service"
    "EXPECT_PARTIALS="
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
  kickos_add_qemu_test(NAME microbit_selftest_p3 TARGET selftest_p3 BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms_p3})
  set_property(TEST microbit_selftest_p3 APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=domain_share,confused_deputy,mem_self_grant"
    "EXPECT_PARTIALS=caller_stack,mmio_grant"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
endif()
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_selftest TARGET selftest BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms})
  set_property(TEST qemu_riscv_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_selftest TARGET selftest BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms})
  set_property(TEST qemu_riscv64_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_ARM64_BOARDS)
  string(REPLACE "-" "_" _tag "${KICKOS_BOARD}")
  kickos_add_qemu_test(NAME ${_tag}_selftest TARGET selftest BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms})
  set_property(TEST ${_tag}_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-x86_64")
  kickos_add_qemu_test(NAME qemu_x86_64_selftest TARGET selftest BOARD qemu-x86_64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms})
  set_property(TEST qemu_x86_64_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu")
  add_test(
    NAME    oot_export_mcu
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_oot_export_mcu.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  # FIXTURES_REQUIRED is named explicitly: without it ctest -j runs cmake --install on the
  # build dir concurrently with kickos_build and fails about one run in ten.
  set_tests_properties(oot_export_mcu PROPERTIES TIMEOUT 120
                       FIXTURES_REQUIRED kickos_build LABELS host)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND KICKOS_ARCH STREQUAL "armv7m")
  add_test(
    NAME    kernel_ctor_placement
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_kernel_ctor_placement.sh"
            "$<TARGET_FILE:selftest>" "${CMAKE_NM}" "${CMAKE_OBJCOPY}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            "$<TARGET_FILE:kickos_lib>")
  set_tests_properties(kernel_ctor_placement PROPERTIES TIMEOUT 60 LABELS host)
endif()

if(KICKOS_BUILD_TESTS AND TARGET selftest)
  # Every archive of the rescan group, so the gate sees the WHOLE set of definitions the link had
  # to choose from; a partial list reads a backend as absent. add_test does NOT split a
  # $<TARGET_OBJECTS:> expansion, so they arrive as one semicolon-joined argument.
  set(_seam_archives "$<TARGET_FILE:kickos_kernel>" "$<TARGET_FILE:kickos_lib>"
                     "$<TARGET_FILE:kickos_user>"
                     "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>")
  if(KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP})
    list(APPEND _seam_archives "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>")
  endif()
  foreach(_lib ${KICKOS_INIT_PROVIDER} ${KICKOS_SERVICE_LIST_LIBS} ${KICKOS_BOARD_PINMAP_LIBS})
    if(TARGET ${_lib})
      get_target_property(_lib_type ${_lib} TYPE)
      if(_lib_type STREQUAL "STATIC_LIBRARY")
        list(APPEND _seam_archives "$<TARGET_FILE:${_lib}>")
      endif()
    endif()
  endforeach()
  list(APPEND _seam_archives "$<TARGET_OBJECTS:selftest>")
  add_test(
    NAME    seam_defaults
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_seam_defaults.sh"
            "${CMAKE_NM}" "${CMAKE_READELF}"
            "${_selftest_elf}" "${_selftest_map}"
            "${PROJECT_SOURCE_DIR}/tests/static/weak_allowlist.txt"
            ${_seam_archives})
  set_tests_properties(seam_defaults PROPERTIES TIMEOUT 120 LABELS host)

  # Driver-class shadowing gate, on the SAME inventory. The last argument before the inventory is
  # 1 when this image compiles the mocks, which is the gate's positive control: it must SEE a
  # class definition on the link line.
  set(_class_expect_app 0)
  if(KICKOS_ENABLE_SELFTEST)
    set(_class_expect_app 1)
  endif()
  add_test(
    NAME    class_backend
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_class_backend.sh"
            "${CMAKE_NM}"
            # Escaped, because CMake splits an unescaped `;` in a COMMAND argument into
            # separate arguments and the positional ones after it would shift.
            "${PROJECT_SOURCE_DIR}/user/include/kickos/driver\;${PROJECT_SOURCE_DIR}/user/include/kickos\;${PROJECT_SOURCE_DIR}/user/include/kickos/sys"
            "${_selftest_map}" "${_class_expect_app}"
            ${_seam_archives})
  set_tests_properties(class_backend PROPERTIES TIMEOUT 120 LABELS host)
endif()

# RISC-V small-data guard: the KickOS libs (built -msmall-data-limit=0) must emit ZERO
# .sdata/.sbss, so gp anchors the app's window and nothing of the kernel's. NECESSARY AND NOT
# SUFFICIENT on the translating board, which is why riscv_kernel_gp is registered beside it: gp
# addressing is MADE BY THE LINKER, so an archive with no .sdata at all can still end up with
# gp-relative kernel accesses in the linked image.
if(KICKOS_BUILD_TESTS
   AND ((KICKOS_HAVE_MPU AND KICKOS_ARCH STREQUAL "rv32imac")
        OR (KICKOS_HAVE_ASPACE AND KICKOS_ARCH STREQUAL "rv64imac")))
  add_test(
    NAME    riscv_no_smalldata
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_riscv_no_smalldata.sh"
            "${CMAKE_OBJDUMP}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            "$<TARGET_FILE:kickos_lib>")
  set_tests_properties(riscv_no_smalldata PROPERTIES TIMEOUT 60 LABELS host)
endif()

# And the LINKED image, which is the only place gp addressing exists: the linker MAKES a
# gp-relative access out of an ordinary upper/lower pair whose target lands inside gp +/- 0x800,
# so a kernel reference to an app-half symbol links silently through the app's own anchor. Three
# images, so a reference the optimiser only emits in one of them is still in the corpus.
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_ASPACE AND KICKOS_ARCH STREQUAL "rv64imac")
  add_test(
    NAME    riscv_kernel_gp
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_riscv_kernel_gp.sh"
            "${CMAKE_OBJDUMP}" ".text"
            "$<TARGET_FILE:selftest>"
            "$<TARGET_FILE:hello>"
            "$<TARGET_FILE:cxxtest>")
  set_tests_properties(riscv_kernel_gp PROPERTIES TIMEOUT 120 LABELS host)
endif()

# The same hazard in its larger form: the app window is at 0x40000000, inside medlow's absolute
# reach, so the linker relaxes such a reference to lui+addi and the link succeeds whether or not
# gp is involved. This one reads the kernel archives' relocations, which name the symbol an
# instruction operand resolves to whatever the linker did to the encoding.
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_ASPACE AND KICKOS_ARCH STREQUAL "rv64imac")
  add_test(
    NAME    riscv_kernel_apphalf
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_riscv_kernel_apphalf.sh"
            "${CMAKE_READELF}"
            "${PROJECT_SOURCE_DIR}/tests/static/riscv_apphalf_allowlist.txt"
            "$<TARGET_FILE:selftest>"
            "$<TARGET_FILE:hello>"
            "$<TARGET_FILE:cxxtest>"
            "--"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>")
  set_tests_properties(riscv_kernel_apphalf PROPERTIES TIMEOUT 120 LABELS host)
endif()

# And the kernel window's own leaves, out of the same three images. The boundary they are held
# against is the image's own section table, so a layout that moved a fetched or a stored section
# across it fails here rather than at the first access.
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_ASPACE AND KICKOS_ARCH STREQUAL "rv64imac")
  add_test(
    NAME    riscv_kernel_wx
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/riscv_kernel_wx.py"
            "${CMAKE_READELF}"
            "$<TARGET_FILE:selftest>"
            "$<TARGET_FILE:hello>"
            "$<TARGET_FILE:cxxtest>")
  set_tests_properties(riscv_kernel_wx PROPERTIES TIMEOUT 120 LABELS host)
endif()

# App-window leak guard for the inverted .appdata scheme: the enforcing linker scripts name the
# CLOSED privileged set (kernel/arch/chip/lib) and .appdata/.appbss catch everything else, so ONE
# renamed selector drops a whole archive into a window domain.cc grants R+W to every
# unprivileged thread, with the script's own ASSERT(_ebss > _sbss) still true. The gate refuses
# an image with no __kickos_appdata_start, which is why sim is out.
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND NOT KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    appdata_no_kernel
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_appdata_no_kernel.sh"
            "${CMAKE_NM}"
            "$<TARGET_FILE:selftest>"
            "${_selftest_map}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            "$<TARGET_FILE:kickos_lib>")
  set_tests_properties(appdata_no_kernel PROPERTIES TIMEOUT 60 LABELS host)
endif()
