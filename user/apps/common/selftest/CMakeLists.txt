# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The core verification app and the CI gates. The app self-asserts and emits TAP over the
# console, linking the freestanding harness in tests/tap directly.

# The posture knobs the image(s) below carry. A list, because on the split parts they have to
# reach TWO targets and one applied to only one of them gives the halves different suites.
set(_selftest_defs "")

# The two `return <int>;` literals cmake/boot_arena.cmake scraped out of the backend .cc this
# link resolves. Unset on the sim, which reaches no scrape without a KICKOS_CHIP and a linker
# script. Both derefs MUST stay quoted: get_property leaves the variable UNSET for a property
# never set, and an unquoted unset name compares as its own literal, so the guard would pass
# and define both macros EMPTY, which still satisfies `#if defined`.
get_property(_mpu_min_region_cfg GLOBAL PROPERTY KICKOS_MPU_MIN_REGION_CFG)
get_property(_mpu_region_pow2_cfg GLOBAL PROPERTY KICKOS_MPU_REGION_POW2_CFG)
if(NOT "${_mpu_min_region_cfg}" STREQUAL "" AND NOT "${_mpu_region_pow2_cfg}" STREQUAL "")
  list(APPEND _selftest_defs
    KICKOS_MPU_MIN_REGION_CFG=${_mpu_min_region_cfg}
    KICKOS_MPU_REGION_POW2_CFG=${_mpu_region_pow2_cfg})
endif()

# microbit's arena holds either the UART service's 1 KiB ring or the arena probes, so the choice
# is pinned by name here rather than left to the allocator.
if(KICKOS_BOARD STREQUAL "microbit")
  list(APPEND _selftest_defs KICKOS_SELFTEST_NO_UART_SERVICE=1)
endif()

# The two 64 KiB FLASH parts (stm32f103 bluepill-c8, stm32f302) and microbit (nrf51) build the
# suite as THREE images: main.cc partitions its registration list into three contiguous regions
# and KICKOS_SELFTEST_PART selects one, so the elided regions' bodies are never emitted. Each
# part plans 1..N from its OWN registry, so the bench has to run all three to cover the suite.
# Undefined elsewhere, where main.cc defaults it to 0 and registers everything.
#
# microbit splits for SRAM and not FLASH, its .bss being what the arena starts after: eliding
# a region's bodies takes their statics out of .bss, so the part's arena base drops and the
# arena probes at the bottom of region 2 fit again.
set(_selftest_split 0)
if(KICKOS_CHIP STREQUAL "stm32f103" OR KICKOS_CHIP STREQUAL "stm32f302"
   OR KICKOS_CHIP STREQUAL "nrf51")
  set(_selftest_split 1)
endif()

# PEAK concurrently held capabilities, not a sum over the run: every arm reclaims before the next
# begins. CAPABILITIES must always fit, being main's 2 permanent caps (g_lock, g_done) plus the
# widest mandatory arm's 3; CAPABILITIES_OPTIONAL is 3 more for t_mutex_deadlock, which holds 2
# mutexes and 4 semaphores at once and SKIPS when it cannot allocate them.
set(_selftest_srcs main.cc "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc")
# spi_mock.cc is a stand-in SPI backend under t_bus_device_slots, gated exactly like that arm.
# No BACKEND keyword: it is the suite's own TU, not a library the fleet selects. It DEFINES the
# public kos_spi_* names while the image may also carry a chip driver, which is legal only
# because the SPI SERVICE drivers rename their class symbols and this mock holds the public
# names alone; tests/static/check_class_backend.sh asserts it per link.
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

# Link map for the seam-fallback gate below: only the map names the archive MEMBER each symbol
# was resolved from, which is what proves a backend's definition beat its fallback.
set(_selftest_map "$<TARGET_FILE:selftest>.map")
target_link_options(selftest PRIVATE "-Wl,-Map=${_selftest_map}")
set_property(TARGET selftest APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${_selftest_map}")

# The expected-skip list every gate below carries; checked by name in
# tests/integration/check_tap_stream.sh.
set(KICKOS_EXPECT_SKIPS "")

# The same for the arms that report PARTIAL. A PARTIAL reports `ok`, so neither the plan/case
# reconciliation nor the skip bookkeeping can see one and this by-name set is the only thing that
# can. It is NOT derivable from _tap_arms: whether a board mints a free DEV window, reserves any
# block or can spare a stack are runtime facts, so the conditions below name the postures.
set(KICKOS_EXPECT_PARTIALS "")
if(KICKOS_ARCH STREQUAL "sim")
  # The host admits one DEV window shape and periph_reg_write_mask holds it, so the free-window
  # arms find nothing to take.
  list(APPEND KICKOS_EXPECT_PARTIALS periph_reg_write_unheld)
endif()
if(KICKOS_ARCH STREQUAL "armv8a")
  # virt_arm64 names no MMIO window a driver could be granted: it declares no reserved blocks
  # and its map keeps the device gigabyte EL1-only, so the arm's free-window half finds
  # nothing to take and reports PARTIAL. That is a real gap in coverage and not a quirk: the
  # held arm runs only where a board mints a window.
  list(APPEND KICKOS_EXPECT_PARTIALS periph_reg_write_unheld)
endif()
# cap_chunk_span needs a table WIDER than the chunk granule (KICKOS_CAP_CHUNK_TARGET) to reach a
# segmented index; below that it runs the flat-decode half and reports PARTIAL. The summed width
# is not readable here, cmake/cap_table.cmake resolving it after this directory is added, but the
# SUPPLY is and the sum is refused above it. cap_child_width is on the same condition: at that
# width the child width IS root's.
if(KICKOS_CAP_TABLE_SUPPLY LESS_EQUAL KICKOS_CAP_CHUNK_TARGET)
  list(APPEND KICKOS_EXPECT_PARTIALS cap_chunk_span cap_child_width)
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  # The reserved-overlap matrix needs at least one arch_reserved_blocks entry. The host and the
  # mps2 parts declare none; the qemu-riscv PMP port does, so it runs the matrix and must not be
  # permitted to drop it.
  if(NOT KICKOS_BOARD STREQUAL "qemu-riscv")
    list(APPEND KICKOS_EXPECT_PARTIALS grant_reserved)
  endif()
  if(KICKOS_ARCH STREQUAL "sim")
    list(APPEND KICKOS_EXPECT_PARTIALS dev_window_exclusive)
  endif()
endif()
# The threads permitted to fault, by NAME. Every other thread-fault record in the stream is an
# arm dying the wrong way, and a contained fault reconciles the plan and the case count, so
# this set is the only thing that can tell the two apart
# (tests/integration/check_tap_stream.sh).
set(KICKOS_EXPECT_FAULTS "")
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION)
  list(APPEND KICKOS_EXPECT_FAULTS fvic)
endif()

# Comma-separated, never semicolons: ENVIRONMENT is itself a CMake list, so a raw list
# deref would split the value into further bogus environment entries.
list(JOIN KICKOS_EXPECT_PARTIALS "," KICKOS_EXPECT_PARTIALS)
list(JOIN KICKOS_EXPECT_FAULTS "," KICKOS_EXPECT_FAULTS)

# The EXACT number of tap::add calls main.cc reaches in this posture. tap.cc plans `1..N` from the
# RUNTIME registry, so plan and case count shrink together when an arm is deleted and only an
# expectation from OUTSIDE the image can see it. The conditions mirror the #if guards around the
# registration block at the bottom of main.cc, one clause per guard.
set(_tap_arms 86) # unconditional, incl. readable_global, endpoint_send_timeout, recv_timeout, timed_arg_refusals, 3 x call_timeout_*, reply_stale_caller, reply_abandoned_cap, thread_join, join_stale_gen, join_timeout, task_handles, task_member_refusals, task_creator_gate, task_group_kill, 3 x thread_slay_*, 2 x task_slay_*
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 14") # bus_device_slots, uart_service, reboot_priv, 11 x irq_*
  math(EXPR _tap_arms "${_tap_arms} + 1") # call_reg_fastpath (region 1)
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
# region_mode reads an MPU region encoding out of two consecutive kos_ram_alloc results. A
# translating backend has neither the encoding nor a monotonic allocator, so the arm is not
# registered there (main.cc, next to its definition).
if(NOT KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms "${_tap_arms} + 1") # region_mode
endif()
if(KICKOS_ARCH STREQUAL "sim")
  math(EXPR _tap_arms "${_tap_arms} + 1") # periph_reg_write_mask
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 29") # aspace_seam, aspace_model, aspace_map_cycle, aspace_translate, aspace_refusals, aspace_span, aspace_balance, aspace_domain_balance, aspace_forced_unwind, aspace_churn, stack_is_frames, 2 x aspace_two_spaces_*, process_private_data, task_siblings_share, task_handoff_readback, task_handoff_donor_exits, self_grant_cross_task, reservation_teardown, parked_frame_hostile, split_access, process_ipc_same_addr, process_call_reply, grant_kernel_word_refused, self_grant_retype, reent_seating, aspace_acquire_balance, map_tlbi_elided, process_data_template
endif()
# T8's containment arm alone: it faults on purpose, so a translating backend that builds no
# resumable frame would panic the suite instead of failing an arm.
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms "${_tap_arms} + 1") # fault_kills_task
endif()

# The same count per region, mirroring the #if guards inside each. The regions are separated by
# the `#undef TAP_ADD` lines in main.cc: region 1 ends after call_donation, region 2 after
# periph_reg_write_mask, region 3 is the rest.
#
# These MUST stay independent expressions, never derived from _tap_arms above: the equality
# asserted below is only a check while both sides are stated separately, and a new arm has to
# move a clause on each side.
set(_tap_arms_p1 48) # region 1, unconditional, incl. endpoint_send_timeout, recv_timeout, timed_arg_refusals, 3 x call_timeout_*, reply_stale_caller, reply_abandoned_cap
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p1 "${_tap_arms_p1} + 1") # call_reg_fastpath: reads the fastpath counter
endif()

set(_tap_arms_p2 19) # region 2, unconditional, incl. readable_global, 3 x call_donation_*, 8 x cap_*, console_publish_priv, shutdown_priv
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p2 "${_tap_arms_p2} + 3") # bus_device_slots, uart_service, reboot_priv
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p2 "${_tap_arms_p2} + 1") # endpoint_bound
endif()
if(KICKOS_ARCH STREQUAL "sim")
  math(EXPR _tap_arms_p2 "${_tap_arms_p2} + 1") # periph_reg_write_mask
endif()

set(_tap_arms_p3 19) # region 3, unconditional, incl. thread_join, join_stale_gen, join_timeout, task_handles, task_member_refusals, task_creator_gate, task_group_kill, 3 x thread_slay_*, 2 x task_slay_*
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 11") # 11 x irq_*
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 2") # grant_reserved, dev_window_exclusive
endif()
if(KICKOS_HAVE_MPU)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # stackbase_arena
endif()
if(KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # caller_stack_arena
endif()
if(NOT KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # region_mode
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 29") # 13 x aspace_* + stack_is_frames + process_private_data + process_data_template + task_siblings_share + task_handoff_readback + task_handoff_donor_exits + self_grant_cross_task + self_grant_retype + reservation_teardown + parked_frame_hostile + split_access + process_ipc_same_addr + process_call_reply + grant_kernel_word_refused + reent_seating + map_tlbi_elided
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms_p3 "${_tap_arms_p3} + 1") # fault_kills_task
endif()

# Totality, checked on EVERY board and not only the ones that split.
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
  # The guard-probe case is registered only under KICKOS_ENABLE_SELFTEST, which the sim preset
  # enables, so the plan count still matches.
  add_test(NAME selftest
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_selftest.sh" "$<TARGET_FILE:selftest>"
            ${_tap_arms})
  set_tests_properties(selftest PROPERTIES TIMEOUT 30)
  set_property(TEST selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")

  # The sim's only coverage of the published console route (kos_send on stdout cap index 0 ->
  # userspace driver). One provider links per image, so the script makes its own build tree,
  # and every input the arm count depends on has to travel with it: a fresh build otherwise
  # takes the board's base variant and every knob default.
  add_test(
    NAME    sim_published_console
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_published.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" ${_tap_arms}
            "${KICKOS_CONFIG_VARIANT}")
  set_tests_properties(sim_published_console PROPERTIES TIMEOUT 300)
  # Its own build tree, at the same board posture, so the same two permission sets.
  set_property(TEST sim_published_console APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")

  # An out-of-tree app builds against the exported KickOS package and runs.
  add_test(
    NAME    oot_export
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_oot_export.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  set_tests_properties(oot_export PROPERTIES TIMEOUT 120)
endif()

# The same TAP suite on the runnable QEMU targets. Needs KICKOS_ENABLE_SELFTEST (the qemu*
# presets enable it); rr_interleave sizes its RR quantum to the target's clock so it survives the
# coarse QEMU semihosting clock.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_selftest TARGET selftest BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms})
  set_property(TEST ${KICKOS_QEMU_MPS2_TAG}_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
endif()
# microbit (nrf51) runs the same suite on armv6m.
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
  # The one skip is a PIN and not an arena outcome: uart_service is held out by
  # KICKOS_SELFTEST_NO_UART_SERVICE at the top of this file, its 1 KiB ring and the arena
  # probes wanting the SAME region.
  #
  # Both lists are a MEASUREMENT and not slack: a listed arm that did NOT skip is only a NOTE,
  # so a stale list quietly permits a regression.
  kickos_add_qemu_test(NAME microbit_selftest_p2 TARGET selftest_p2 BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms_p2})
  set_property(TEST microbit_selftest_p2 APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=uart_service"
    "EXPECT_PARTIALS="
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
  # The three arena skips are the SUITE outgrowing this part, not the part failing: each wants
  # concurrent workers past what 32 KiB seats once the per-thread struct _reent array is below
  # the arena.
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

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_selftest TARGET selftest BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh" ARGS ${_tap_arms})
  set_property(TEST qemu_arm64_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
endif()

# The same for a bare-metal package: install it, then build an out-of-tree app against it with
# the SHIPPED cross toolchain. Build-only, an MCU ELF not being runnable on the host.
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

# Boot-ctor-split guard: on an enforced armv7m link (.kickos_app_init_array exists), assert no
# closed-set kernel ctor leaked into the app-ctor window, where it would run after kmain. A plain
# app is enough: the check needs the section symbols plus the four kernel archives.
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

# Seam-fallback gate: every optional arch/chip seam resolves by archive-member extraction, not by
# __attribute__((weak)). Runs on every board, the resolution being per-link and a board only
# covered by its OWN build tree.
if(KICKOS_BUILD_TESTS AND TARGET selftest)
  # Every archive of the rescan group, so the gate sees the WHOLE set of definitions the link had
  # to choose from; a partial list reads a backend as absent. The app's own objects come too, an
  # app being able to define a seam itself (KICKOS_APP_AUTHORITY does). add_test does NOT split a
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
            "$<TARGET_FILE:selftest>" "${_selftest_map}"
            "${PROJECT_SOURCE_DIR}/tests/static/weak_allowlist.txt"
            ${_seam_archives})
  set_tests_properties(seam_defaults PROPERTIES TIMEOUT 120 LABELS host)

  # Driver-class shadowing gate, on the SAME inventory: whether a real backend's public class
  # symbols were answered by the mocks instead. The last argument before the inventory is 1 when
  # this image compiles the mocks, which is the gate's positive control: it must SEE a class
  # definition on the link line.
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

# RISC-V-under-PMP small-data guard: the gp window sits inside the app grant, so the KickOS libs
# (built -msmall-data-limit=0) must emit ZERO .sdata/.sbss. A stray kernel global there is
# reachable and writable by an unprivileged thread (docs/design-riscv-gp-split.md, Option 4).
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND KICKOS_ARCH STREQUAL "rv32imac")
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

# App-window leak guard for the inverted .appdata scheme: the enforcing linker scripts name the
# CLOSED privileged set (kernel/arch/chip/lib) and .appdata/.appbss catch everything else, so ONE
# renamed selector drops a whole archive into a window domain.cc grants R+W to every unprivileged
# thread, with the script's own ASSERT(_ebss > _sbss) still true. Not registered on sim:
# KICKOS_HAVE_MPU is on there but no linker script carves the window, and the gate refuses an
# image with no __kickos_appdata_start.
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
