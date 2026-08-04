# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The M0 verification app + the CI gates. Links the freestanding TAP harness
# (tests/tap) directly; the app self-asserts and emits TAP over the console.

# The posture knobs the image(s) below carry. Collected into a list first: on the split
# parts they have to reach TWO targets, and one applied to only one of them would give
# the two halves different suites.
set(_selftest_defs "")

# The configure-time region-encoding seam: the two `return <int>;` literals
# cmake/boot_arena.cmake scraped out of the backend .cc this link resolves.
# Unset on the sim, which never reaches the scrape without a KICKOS_CHIP and a linker
# script; region_mode then falls back to a weaker self-consistency check.
# Both derefs MUST stay quoted: get_property leaves the variable UNSET for a property
# that was never set, and an unquoted unset name compares as its own literal, so the
# guard would pass and define both macros EMPTY, which still satisfies `#if defined`.
get_property(_mpu_min_region_cfg GLOBAL PROPERTY KICKOS_MPU_MIN_REGION_CFG)
get_property(_mpu_region_pow2_cfg GLOBAL PROPERTY KICKOS_MPU_REGION_POW2_CFG)
if(NOT "${_mpu_min_region_cfg}" STREQUAL "" AND NOT "${_mpu_region_pow2_cfg}" STREQUAL "")
  list(APPEND _selftest_defs
    KICKOS_MPU_MIN_REGION_CFG=${_mpu_min_region_cfg}
    KICKOS_MPU_REGION_POW2_CFG=${_mpu_region_pow2_cfg})
endif()

# microbit is the fleet's only armv6m no-ring CI gate, and its 16 KiB arena cannot hold both
# the UART service's 1 KiB shared ring block and the four shape-specific arena probes
# (endpoint_crossdomain, mem_self_grant_nonpow2, region_mode, mem_self_grant). uart_service is
# already covered on sim and on every larger board, so running it here would buy duplicate
# coverage and cost four gated cases. Refuse it BY NAME rather than let the arena decide: left
# to the allocator it flips with a few tens of bytes of unrelated static demand.
if(KICKOS_BOARD STREQUAL "microbit")
  list(APPEND _selftest_defs KICKOS_SELFTEST_NO_UART_SERVICE=1)
endif()

# The two 64 KiB FLASH parts (stm32f103 bluepill-c8, stm32f302) build the suite as TWO
# images: main.cc partitions its registration list into two contiguous regions and
# KICKOS_SELFTEST_PART selects one, so the elided region's bodies are never emitted.
# `selftest` is part 1 and `selftest_p2` is part 2, each planning 1..N from its own
# registry, so the bench has to run both to cover the suite.
#
# Undefined on every other board; main.cc then defaults it to 0 and registers everything.
set(_selftest_split 0)
if(KICKOS_CHIP STREQUAL "stm32f103" OR KICKOS_CHIP STREQUAL "stm32f302")
  set(_selftest_split 1)
endif()

# The suite's PEAK concurrently held capabilities, which only the suite knows. CAPABILITIES
# must always fit: main's 2 permanent caps (g_lock, g_done) plus the widest mandatory arm's
# 3 own-caps. CAPABILITIES_OPTIONAL is 3 more for t_mutex_deadlock, which holds 2 mutexes and
# 4 semaphores at once, reclaims all six, and skips when it cannot allocate them - so a board
# that cannot grant them stays a target for the rest of the suite. Neither is a sum over the
# run: every other arm reclaims before the next begins.
set(_selftest_srcs main.cc "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc")
kickos_add_application(selftest SOURCES ${_selftest_srcs} BOARD ${KICKOS_BOARD}
  CAPABILITIES 5 CAPABILITIES_OPTIONAL 3)
set(_selftest_images selftest)
if(_selftest_split)
  kickos_add_application(selftest_p2 SOURCES ${_selftest_srcs} BOARD ${KICKOS_BOARD}
    CAPABILITIES 5 CAPABILITIES_OPTIONAL 3)
  list(APPEND _selftest_images selftest_p2)
  target_compile_definitions(selftest    PRIVATE KICKOS_SELFTEST_PART=1)
  target_compile_definitions(selftest_p2 PRIVATE KICKOS_SELFTEST_PART=2)
endif()

foreach(_img IN LISTS _selftest_images)
  target_include_directories(${_img} PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")
  target_compile_definitions(${_img} PRIVATE ${_selftest_defs})
  # Size-optimise selftest's OWN TUs: at the fleet Debug default its .text overflows the
  # 64K-flash parts (stm32f103 bluepill-c8, stm32f302). Only this app's TUs are affected,
  # so the kernel/arch/lib archives stay at the board build type.
  if(NOT (KICKOS_ARCH STREQUAL "sim"))
    target_compile_options(${_img} PRIVATE -Os)
  endif()
endforeach()

# Link map for the seam-fallback gate below: only the map names the archive MEMBER each
# symbol was resolved from, which is what proves a backend's definition beat its
# fallback. Byproduct, so a clean removes it.
set(_selftest_map "$<TARGET_FILE:selftest>.map")
target_link_options(selftest PRIVATE "-Wl,-Map=${_selftest_map}")
set_property(TARGET selftest APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${_selftest_map}")

# The expected-skip list every gate below carries (EXPECT_SKIPS, checked by name in
# tests/check_tap_stream.sh). Empty means "no test may skip", which is every board here
# except microbit (its own hardcoded list further down).
set(KICKOS_EXPECT_SKIPS "")

# The same, for the arms that report PARTIAL: they ran their invariant and left a sub-case
# unexercised here. A PARTIAL reports `ok`, so neither the plan/case reconciliation nor the
# skip bookkeeping can see one and this by-name set is the only thing that can. It is NOT
# derivable from _tap_arms: whether a board mints a free DEV window, reserves any block, or
# can spare a stack are runtime facts, so the conditions below name the postures measured.
set(KICKOS_EXPECT_PARTIALS "")
if(KICKOS_ARCH STREQUAL "sim")
  # The host admits exactly one DEV window shape and periph_reg_write_mask already holds
  # it, so the free-window arms find nothing to take.
  list(APPEND KICKOS_EXPECT_PARTIALS periph_reg_write_unheld)
endif()
# cap_chunk_span needs a table WIDER than the chunk granule (KCAP_CHUNK_TARGET) to reach a
# segmented index at all; below that it runs the flat-decode half and reports PARTIAL. The
# summed width is not a variable here, because cmake/cap_table.cmake resolves it after this
# directory is added, so the condition is the chips whose KICKOS_CAP_TABLE_SUPPLY is 7.
# Changing the granule changes which boards land here.
if(KICKOS_CHIP STREQUAL "nrf51" OR KICKOS_CHIP STREQUAL "stm32f103"
   OR KICKOS_CHIP STREQUAL "stm32f302")
  list(APPEND KICKOS_EXPECT_PARTIALS cap_chunk_span)
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  # The reserved-overlap matrix needs the board to declare at least one
  # arch_reserved_blocks entry. The host and the mps2 parts declare none; the qemu-riscv
  # PMP port does, so it runs the matrix and must not be permitted to drop it.
  if(NOT KICKOS_BOARD STREQUAL "qemu-riscv")
    list(APPEND KICKOS_EXPECT_PARTIALS grant_reserved)
  endif()
  if(KICKOS_ARCH STREQUAL "sim")
    list(APPEND KICKOS_EXPECT_PARTIALS dev_window_exclusive)
  endif()
endif()
# Comma-separated, never semicolons: ENVIRONMENT is itself a CMake list, so a raw list
# deref would split the value into further bogus environment entries.
list(JOIN KICKOS_EXPECT_PARTIALS "," KICKOS_EXPECT_PARTIALS)

# The EXACT number of tap::add calls main.cc reaches in this posture, handed to every
# gate below. tap.cc plans `1..N` from the RUNTIME registry, so plan and case count
# shrink together when an arm is deleted and no self-consistent parse can see it; only
# an expectation from OUTSIDE the image can. The conditions mirror the #if guards around
# the registration block at the bottom of main.cc, one clause per guard.
set(_tap_arms 61) # unconditional
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 14") # bus_device_slots, uart_service, reboot_priv, 11 x irq_*
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 3") # endpoint_bound, grant_reserved, dev_window_exclusive
endif()
if(KICKOS_HAVE_MPU)
  math(EXPR _tap_arms "${_tap_arms} + 1") # stackbase_arena
endif()
if(KICKOS_ARCH STREQUAL "sim")
  math(EXPR _tap_arms "${_tap_arms} + 1") # periph_reg_write_mask
endif()

# The same count, per region of the split list, mirroring the #if guards inside each
# region. Region 1 ends at the `#undef TAP_ADD` after call_donation_pending in main.cc;
# region 2 is everything below it.
#
# These two MUST stay independent expressions, never derived from _tap_arms above: the
# equality asserted below is only a check while both sides are stated separately, and a
# new arm has to move a clause on each side.
set(_tap_arms_p1 43) # every arm in region 1

set(_tap_arms_p2 18) # region 2, unconditional
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p2 "${_tap_arms_p2} + 14") # bus_device_slots, uart_service, reboot_priv, 11 x irq_*
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_p2 "${_tap_arms_p2} + 3") # endpoint_bound, grant_reserved, dev_window_exclusive
endif()
if(KICKOS_HAVE_MPU)
  math(EXPR _tap_arms_p2 "${_tap_arms_p2} + 1") # stackbase_arena
endif()
if(KICKOS_ARCH STREQUAL "sim")
  math(EXPR _tap_arms_p2 "${_tap_arms_p2} + 1") # periph_reg_write_mask
endif()

# Totality. Checked on EVERY board, not only the ones that split, so every posture in
# the fleet exercises it.
math(EXPR _tap_arms_split "${_tap_arms_p1} + ${_tap_arms_p2}")
if(NOT _tap_arms_split EQUAL _tap_arms)
  message(FATAL_ERROR
    "selftest arm partition is not total: part 1 (${_tap_arms_p1}) + part 2 "
    "(${_tap_arms_p2}) = ${_tap_arms_split}, but the whole suite registers ${_tap_arms} "
    "arms in this posture. An arm was added, deleted or moved across the region boundary "
    "in main.cc without its per-part clause moving with it. Fix the counts in "
    "${CMAKE_CURRENT_LIST_FILE}, do not relax this check.")
endif()

if(_selftest_split)
  message(STATUS
    "selftest: split into two images -- selftest plans ${_tap_arms_p1} arms, "
    "selftest_p2 plans ${_tap_arms_p2} (${_tap_arms} together)")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  # The guard-probe case is registered only under KICKOS_ENABLE_SELFTEST (the sim
  # preset enables it), so the plan count still matches.
  add_test(NAME selftest
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_sim_selftest.sh" "$<TARGET_FILE:selftest>"
            ${_tap_arms})
  set_tests_properties(selftest PROPERTIES TIMEOUT 30)
  set_property(TEST selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}")

  # The OTHER console posture: the sim's only coverage of the published route
  # (kos_send on stdout cap index 0 -> userspace driver). One provider links per image,
  # so it needs its own build tree; the script makes one. KICKOS_HAVE_MPU has to travel
  # with the arm count: the script's `--preset sim` re-derives that knob, and the sim
  # defaults it to 1, so an outer -DKICKOS_HAVE_MPU=0 otherwise checks a posture-1 stream
  # against a posture-0 expectation.
  add_test(
    NAME    sim_published_console
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_sim_published.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" ${_tap_arms} ${KICKOS_HAVE_MPU})
  set_tests_properties(sim_published_console PROPERTIES TIMEOUT 300)
  # Its own build tree, at the same board posture, so the same two permission sets.
  set_property(TEST sim_published_console APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}")

  # Dependency-inversion acceptance: an out-of-tree app builds against the
  # exported KickOS package (find_package + kickos_add_application) and runs.
  add_test(
    NAME    oot_export
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_oot_export.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  set_tests_properties(oot_export PROPERTIES TIMEOUT 120)
endif()

# The same TAP suite on the runnable QEMU targets. Needs KICKOS_ENABLE_SELFTEST (the
# qemu* presets enable it); rr_interleave sizes its RR quantum to the target's clock so
# it survives the coarse QEMU semihosting clock.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_selftest TARGET selftest BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_selftest.sh" ARGS ${_tap_arms})
  set_property(TEST ${KICKOS_QEMU_MPS2_TAG}_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}")
endif()
# microbit (nrf51) runs the same suite on armv6m with MAX_SEMAPHORES=4 / MAX_THREADS=2.
# Every test must reclaim its semaphores so peak concurrent stays within 4.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_selftest TARGET selftest BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_selftest.sh" ARGS ${_tap_arms})
  # The only board with legitimate skips. MAX_THREADS=2 / MAX_SEMAPHORES=4 cannot host
  # the multi-worker choreographies:
  #   mutex_basic, mutex_pi_donation, mutex_chain_boost, mutex_deadlock,
  #   mutex_multi_held: 3-worker PI choreographies on a 2-slot pool
  #   call_infoless_revert (4 workers), call_donation (3)
  #   call_donation_hold, call_donation_slow, call_donation_pending: 3 workers each
  #   domain_share: arena cannot spare the shared region
  #   irq_as_event: wants a 4 KiB MMIO page out of 16 KiB SRAM
  #   uart_service: PINNED above (KICKOS_SELFTEST_NO_UART_SERVICE), no longer an arena outcome
  # ARENA CLIFF, and the reason uart_service is pinned rather than left to the allocator: this
  # board sits close enough to the edge that a few tens of bytes of static demand, in EITHER
  # direction, flips one of endpoint_crossdomain, mem_self_grant_nonpow2, region_mode or
  # mem_self_grant between running and skipped. Before changing any static demand here, diff
  # the whole TAP output and not the verdict: the arm that flips is the cause.
  # Three more arms run their invariant and drop a sub-case, reporting PARTIAL: they count
  # as passes, not skips, and carry their own permission set.
  #   caller_stack: the arena cannot spare a caller-owned stack
  #   confused_deputy: the arena cannot spare the grandchild's stack
  #   cap_chunk_span: a 7-slot table compiles the flat decode, so no segmented index exists
  # Comma-separated, never semicolons: ENVIRONMENT is itself a CMake list, so a semicolon
  # would split the value into a second bogus environment entry.
  set_property(TEST microbit_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=mutex_basic,mutex_pi_donation,mutex_chain_boost,mutex_deadlock,mutex_multi_held,call_infoless_revert,call_donation,call_donation_hold,call_donation_slow,call_donation_pending,domain_share,irq_as_event,uart_service"
    "EXPECT_PARTIALS=caller_stack,confused_deputy,cap_chunk_span")
endif()
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_selftest TARGET selftest BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_selftest.sh" ARGS ${_tap_arms})
  set_property(TEST qemu_riscv_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}")
endif()

# The same acceptance criterion for a bare-metal package: install the MCU
# package, then build an out-of-tree app against it with the SHIPPED cross
# toolchain (build-only; an MCU ELF can't run on the host). Registered on the
# qemu board -- the runnable ARM CI target -- so the MCU packaging surface
# (shipped toolchain + board.cmake, the INSTALL_INTERFACE linker script, the
# single-board guard) gets real coverage the sim gate can't give.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu")
  add_test(
    NAME    oot_export_mcu
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_oot_export_mcu.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  # Registered with a bare add_test rather than through the wrapper, so it needs the
  # build fixture named here: without it ctest -j runs cmake --install on the build
  # dir concurrently with kickos_build and fails about one run in ten.
  set_tests_properties(oot_export_mcu PROPERTIES TIMEOUT 120
                       FIXTURES_REQUIRED kickos_build)
endif()

# Boot-ctor-split guard: on an enforced armv7m link (.kickos_app_init_array exists),
# assert no closed-set kernel ctor leaked into the app-ctor window (would run after
# kmain). Runs against this selftest ELF -- a plain app is enough: the check needs
# the section symbols plus the four kernel archives, not app/libstdc++ ctors.
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND KICKOS_ARCH STREQUAL "armv7m")
  add_test(
    NAME    kernel_ctor_placement
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_kernel_ctor_placement.sh"
            "$<TARGET_FILE:selftest>" "${CMAKE_NM}" "${CMAKE_OBJCOPY}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            "$<TARGET_FILE:kickos_lib>")
  set_tests_properties(kernel_ctor_placement PROPERTIES TIMEOUT 60)
endif()

# Seam-fallback gate: every optional arch/chip seam resolves by archive-member
# extraction, not by __attribute__((weak)). Runs on every board, because the resolution it
# checks is per-link and a board is only covered by its OWN build tree. Reads this
# selftest ELF plus its link map (only the map names the member each symbol came from).
if(KICKOS_BUILD_TESTS AND TARGET selftest)
  # Every archive of the rescan group, so the gate sees the WHOLE set of definitions the
  # link had to choose from; a partial list would read a backend as absent. The app's own
  # objects come too: they are on the link command line (always in the image) and an app
  # may define a seam itself, KICKOS_APP_AUTHORITY being the one that does. add_test does
  # NOT split a $<TARGET_OBJECTS:> expansion, so they reach the script as one
  # semicolon-joined argument, which it splits back apart.
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
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_seam_defaults.sh"
            "${CMAKE_NM}" "${CMAKE_READELF}"
            "$<TARGET_FILE:selftest>" "${_selftest_map}"
            "${PROJECT_SOURCE_DIR}/tests/weak_allowlist.txt"
            ${_seam_archives})
  set_tests_properties(seam_defaults PROPERTIES TIMEOUT 120)
endif()

# RISC-V-under-PMP small-data guard: the gp window sits inside the app grant, so the
# KickOS libs (built -msmall-data-limit=0) must emit ZERO .sdata/.sbss -- a stray
# kernel global there is reachable/writable by an unprivileged thread. Assert per
# archive (docs/design-riscv-gp-split.md, Option 4).
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND KICKOS_ARCH STREQUAL "rv32imac")
  add_test(
    NAME    riscv_no_smalldata
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_riscv_no_smalldata.sh"
            "${CMAKE_OBJDUMP}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            "$<TARGET_FILE:kickos_lib>")
  set_tests_properties(riscv_no_smalldata PROPERTIES TIMEOUT 60)
endif()
