# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding `selftest`. Two kinds: the TAP suite booted per board, and the host readers
# that inspect the linked image and the archives it chose its definitions from. The second kind
# takes selftest because it is the image that links the widest set of them.

if(NOT TARGET selftest)
  return()
endif()

# What the app recorded about itself: the arm count each image plans, and the ELF and map this
# link actually produced. Read off the target rather than restated here, so nothing on the test
# side can drift from what was built.
get_target_property(_selftest_elf selftest KICKOS_IMAGE_ELF)
get_target_property(_selftest_map selftest KICKOS_IMAGE_MAP)
get_target_property(_selftest_arms selftest KICKOS_TAP_ARMS)

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
    reply_recv_notify reply_recv_notify_park
    cap_reply_bound_fast cap_reply_bound_slow thread_slay_timeout
    mutex_owner_died_nowaiter aspace_two_spaces_same_grant
    parked_frame_hostile)
endif()

# The twelve-core bench image provisions enough thread slots for its crowded
# placement checks. The refusal arm caps its parked children at 24, so that
# image can legitimately reach the cap before exhausting the larger pool.
if(KICKOS_CONFIG_VARIANT STREQUAL "benchsmp12")
  list(APPEND KICKOS_EXPECT_SKIPS spawn_refusal_frees_task)
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
# reent_per_thread_cores holds one checker and two switchers at once.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1 AND KICKOS_MAX_THREADS LESS 3)
  list(APPEND KICKOS_EXPECT_SKIPS reent_per_thread_cores)
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1 AND NOT KICKOS_LIBC_REENT)
  list(APPEND KICKOS_EXPECT_SKIPS reent_per_thread_cores)
endif()

# Allow peer-dependent tests to skip when an AMP image runs alone. The merged
# partition gate requires them to run when a peer is available.
# Far-reply guard tests need an unanswered port: a node binds all its ports but
# serves only the first. Other nodes can use its spare; the owner cannot.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  list(APPEND KICKOS_EXPECT_SKIPS amp_far_call amp_far_reply_guard amp_far_reply_empty)
endif()

# amp_far_deliver_fault copies a far arrival into a LOCAL thread's buffer and takes that page
# away under the parked thread, so it needs a backend that translates: a region board's
# access_copy is an unconditional kmemcpy and refuses nothing, which leaves the overlap arm as
# that board's only reachable refusal. DERIVED from the backend and not a posture list.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE AND NOT KICKOS_HAVE_ASPACE)
  list(APPEND KICKOS_EXPECT_SKIPS amp_far_deliver_fault)
endif()

# amp_reply_reserve skips on the OPPOSITE posture to the three above, which is why it is its own
# predicate rather than another name on that list. It holds the reply ring toward a peer full so a
# take has no slot to reserve; under one image the peers are this image's own cores and drain
# their own rings, so the forge DECLINES rather than fabricating a state they would act on.
# amp_far_reset_answers and amp_far_answer_deferred decline on the same posture and for the same
# reason: each stomps a peer's call ring and holds the reply ring toward it, and under one image
# those peers are this image's own cores, draining both underneath. amp_far_tail_recovery is NOT
# here, driving the self ring no node produces into, so it runs on every posture.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE AND NOT KICKOS_AMP_OWN_IMAGE)
  list(APPEND KICKOS_EXPECT_SKIPS amp_reply_reserve amp_far_reset_answers
       amp_far_answer_deferred)
endif()

# amp_deferred_doorbell requires a published per-core doorbell destination.
# RP2350 uses a direct SIO write and returns ARCH_IPI_SEAT_NONE, so the test
# skips. Keep this exception limited to backends without a destination slot.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE AND KICKOS_CHIP STREQUAL "rp2350")
  list(APPEND KICKOS_EXPECT_SKIPS amp_deferred_doorbell)
endif()

# The same for the arms that report PARTIAL. A PARTIAL reports `ok`, so neither the plan/case
# reconciliation nor the skip bookkeeping can see one and this by-name set is the only thing
# that can. It is NOT derivable from the arm count: the conditions below name the postures.
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
# irq_kernel_line_reserved tests both claim and injection rejection for a
# kernel-reserved line. Report PARTIAL when none exists. GIC and RP2350 have
# such a line when the image uses doorbells. Test KICKOS_NUM_CORES because
# AMP images also need doorbells even with one kernel core.
if(KICKOS_ENABLE_SELFTEST
   AND NOT ((KICKOS_ARCH STREQUAL "armv8a" OR KICKOS_CHIP STREQUAL "rp2350")
            AND (KICKOS_NUM_CORES GREATER 1 OR KICKOS_AMP_NODE)))
  list(APPEND KICKOS_EXPECT_PARTIALS irq_kernel_line_reserved)
  # The partitioned board's sets are literals further down and cannot read this list, which
  # is JOINed into one string below. Carry the decision, not the membership.
  set(_selftest_kernel_line_partial 1)
endif()
# periph_reg_write_unheld on every backend whose peripheral model cannot witness the refusal.
if(KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "armv8a"
   OR KICKOS_ARCH STREQUAL "rv64imac" OR KICKOS_ARCH STREQUAL "x86_64")
  list(APPEND KICKOS_EXPECT_PARTIALS periph_reg_write_unheld)
endif()
# cap_chunk_span needs a table WIDER than the chunk granule (KICKOS_CAP_CHUNK_TARGET) to reach a
# segmented index. The summed width is not readable here, cmake/cap_table.cmake resolving it
# after this directory is added, but the SUPPLY is. cap_child_width is on the same condition.
if(KICKOS_CAP_TABLE_SUPPLY LESS_EQUAL KICKOS_CAP_CHUNK_TARGET)
  list(APPEND KICKOS_EXPECT_PARTIALS cap_chunk_span cap_child_width)
endif()
# It also needs a free slot BELOW the granule, and root's own creates start above everything
# seated at init: the reserved indices, one capability per partition crossing, and the two
# main() holds for the run. Widening the table does not take that back, the width sum adding
# slots at the top.
set(_selftest_lifetime_caps 2) # g_lock and g_done, created in main() and never closed
math(EXPR _selftest_cap_seated
     "${KICKOS_CAP_FIRST_DYNAMIC} + ${KICKOS_AMP_PORT_COUNT} + ${_selftest_lifetime_caps}")
if(NOT _selftest_cap_seated LESS KICKOS_CAP_CHUNK_TARGET)
  list(APPEND KICKOS_EXPECT_PARTIALS cap_chunk_span)
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
set(_selftest_env
  "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
  "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")

# THE SAME EXPECTATIONS WHERE A CAPTURE OFF SILICON CAN READ THEM. A board with no emulator
# reaches no entry below, so until now nothing judged its TAP stream at all and
# tools/bench/bench-capture.sh counted `ok` lines instead: a capture that LOST lines has fewer
# of them, so the count shrank with the loss while the harness's own trailer kept the truth, and
# the reader outvoted the producer. The manifest is what lets the bench path run
# tests/integration/check_tap_stream.sh with the arguments this file would have given it.
#
# NOTHING IS STATED HERE THAT IS NOT ALREADY STATED: the arm count comes off each image target,
# where the app recorded it, and the three sets are the very variables handed to the entries
# below. The permission sets are per BOARD and derived from that board's own knobs; the only
# per-IMAGE figure is the arm count, and a split image plans its own.
#
# One row per image, `|` separated because the sets are comma-joined and any of them may be
# empty.
get_property(_selftest_manifest_images GLOBAL PROPERTY KICKOS_SELFTEST_IMAGES)
set(_selftest_manifest "")
foreach(_mf_img IN LISTS _selftest_manifest_images)
  get_target_property(_mf_arms ${_mf_img} KICKOS_TAP_ARMS)
  # get_target_property hands back `<var>-NOTFOUND` for a property never set, which is a
  # non-empty string: written out it would reach the capture as an arm count and be compared
  # against the plan as one.
  if(NOT _mf_arms MATCHES "^[0-9]+$")
    message(FATAL_ERROR
      "selftest: the image ${_mf_img} records no KICKOS_TAP_ARMS, so a capture of it could be "
      "checked against nothing. user/apps/common/selftest/CMakeLists.txt sets it per image.")
  endif()
  string(APPEND _selftest_manifest "${_mf_img}|${_mf_arms}|${KICKOS_EXPECT_SKIPS}|"
                                   "${KICKOS_EXPECT_PARTIALS}|${KICKOS_EXPECT_FAULTS}\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/kickos-selftest-manifest.txt" "${_selftest_manifest}")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME selftest
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_selftest.sh" "${_selftest_elf}"
            ${_selftest_arms})
  set_tests_properties(selftest PROPERTIES TIMEOUT 30)
  set_property(TEST selftest APPEND PROPERTY ENVIRONMENT ${_selftest_env})

  # The sim's only coverage of the published console route. One provider links per image, so the
  # script makes its own build tree, and every input the arm count depends on has to travel with
  # it: a fresh build otherwise takes the board's base variant and every knob default.
  add_test(
    NAME    sim_published_console
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_published.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" ${_selftest_arms}
            "${KICKOS_CONFIG_VARIANT}")
  set_tests_properties(sim_published_console PROPERTIES TIMEOUT 300)
  set_property(TEST sim_published_console APPEND PROPERTY ENVIRONMENT ${_selftest_env})

endif()

# Register each board's self-test image and expected arm count. A split board runs one image
# per part; microbit is the only one of them with a QEMU machine, so it is the only one whose
# later parts reach a gate here.
# Keep TIMEOUT above check_qemu_selftest.sh's 180-second limit so the script
# can report the failure before CTest stops it.
if(NOT KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(TARGET selftest
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh"
    TIMEOUT 240
    ARGS ${_selftest_arms})
  if(TEST ${_tag}_selftest)
    set_property(TEST ${_tag}_selftest APPEND PROPERTY ENVIRONMENT ${_selftest_env})
  endif()
endif()

if(KICKOS_BOARD STREQUAL "microbit")
  # The sets below are partitioned BY REGION, NOT BY NAME: an arm belongs to the region whose
  # `#undef TAP_ADD` bounds hold its TAP_ADD line in main.cc, and an image declares the UNION
  # over the regions it carries. check_tap_stream.sh reports a name declared in the wrong
  # image as a NOTE and not a failure, so only that rule catches a mistake.
  #
  # Both lists are a MEASUREMENT and not slack: a listed arm that did NOT skip is only a NOTE,
  # so a stale list quietly permits a regression.
  set(_mb_skips_r1 "")
  set(_mb_partials_r1 "")
  set(_mb_skips_r2 "")
  set(_mb_partials_r2 "")
  set(_mb_skips_r3 uart_service)
  set(_mb_partials_r3 "")
  # irq_as_event asks the arena for a 4 KiB MMIO page after the suite's threads have taken
  # their stacks from it, and on 32 KiB it no longer fits: this board's console TX ring costs
  # 256 bytes of .bss, which is what that page stood on. The arm sees the alloc fail and skips
  # itself by name. Measured: a 128-byte ring restores it, and that needs a 64-byte line
  # bound, below the fault reporter's own KDIAG_FAULT_LINE_MAX.
  # mem_self_grant is NOT here: its decline became a vacuity skip, which is permitted and
  # never expected, so a name for it in this list could never fire and would sit widening the
  # permission. The arm still declines on this board; it declines in the other category.
  set(_mb_skips_r4 domain_share confused_deputy irq_as_event)
  set(_mb_partials_r4 caller_stack mmio_grant)
  # DERIVED from the decision above rather than restated: these sets are literals, so a
  # permission appended to the whole-suite list reached every other board and not this one,
  # and a partitioned board's gate reported that as a failure. Region 4 holds the arm's
  # TAP_ADD line, so this is the region that carries it.
  if(_selftest_kernel_line_partial)
    list(APPEND _mb_partials_r4 irq_kernel_line_reserved)
  endif()
  # The image list and each image's run of regions come off the targets the app declared, so
  # nothing here states again how many images this board ships or which arms are in one.
  get_property(_selftest_images GLOBAL PROPERTY KICKOS_SELFTEST_IMAGES)
  # This board's rows replace the ones written above, for the same reason its entries take
  # these sets rather than the fleet-wide ones: a manifest that disagreed with the gate would
  # be a second answer to the same question.
  set(_selftest_manifest "")
  foreach(_img IN LISTS _selftest_images)
    get_target_property(_mb_arms ${_img} KICKOS_TAP_ARMS)
    get_target_property(_mb_lo ${_img} KICKOS_SELFTEST_FIRST_REGION)
    get_target_property(_mb_hi ${_img} KICKOS_SELFTEST_LAST_REGION)
    set(_mb_skips "")
    set(_mb_partials "")
    foreach(_mb_region RANGE ${_mb_lo} ${_mb_hi})
      if(NOT DEFINED _mb_skips_r${_mb_region} OR NOT DEFINED _mb_partials_r${_mb_region})
        message(FATAL_ERROR
          "selftest: ${_img} carries region ${_mb_region}, which declares no skip or partial "
          "set in ${CMAKE_CURRENT_LIST_FILE}. An undeclared region reads as permitting "
          "nothing, so an arm that has always skipped there would fail this board's gate.")
      endif()
      list(APPEND _mb_skips ${_mb_skips_r${_mb_region}})
      list(APPEND _mb_partials ${_mb_partials_r${_mb_region}})
    endforeach()
    # Comma-separated for the same reason the fleet-wide sets above are.
    list(JOIN _mb_skips "," _mb_skips)
    list(JOIN _mb_partials "," _mb_partials)
    kickos_add_qemu_test(TARGET ${_img}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh"
      TIMEOUT 240
      ARGS ${_mb_arms})
    set_property(TEST microbit_${_img} APPEND PROPERTY ENVIRONMENT
      "EXPECT_SKIPS=${_mb_skips}"
      "EXPECT_PARTIALS=${_mb_partials}"
      "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
    string(APPEND _selftest_manifest "${_img}|${_mb_arms}|${_mb_skips}|"
                                     "${_mb_partials}|${KICKOS_EXPECT_FAULTS}\n")
  endforeach()
  file(WRITE "${CMAKE_BINARY_DIR}/kickos-selftest-manifest.txt" "${_selftest_manifest}")
endif()

# The out-of-tree package gate, on ONE BOARD PER KICKOS_ARCH. Registered on two of the
# seventy-one presets it leaves every arch-private installed header unexamined.
#
# The map is data (tests/integration/oot_arch_boards.txt) and tests/static/check_oot_arch_cover.sh
# holds it whole against boards/*/board.cmake, so a new arch cannot quietly get no row.
set(_oot_map "${PROJECT_SOURCE_DIR}/tests/integration/oot_arch_boards.txt")
# A literal space, never [ \t]: CMake's regex engine has no tab escape, so the class would
# silently reduce to "space or the letter t". check_oot_arch_cover.sh refuses a tab anywhere in
# the map for that reason.
file(STRINGS "${_oot_map}" _oot_rows REGEX "^covers ")
if(_oot_rows STREQUAL "")
  message(FATAL_ERROR "${_oot_map} states no covers row, so the out-of-tree package gate "
                      "would register on no board at all")
endif()
set(_oot_board "")
foreach(_oot_row IN LISTS _oot_rows)
  if(_oot_row MATCHES "^covers +([A-Za-z0-9_]+) +([A-Za-z0-9_-]+)")
    if(CMAKE_MATCH_1 STREQUAL KICKOS_ARCH)
      set(_oot_board "${CMAKE_MATCH_2}")
    endif()
  endif()
endforeach()

if(_oot_board AND KICKOS_BOARD STREQUAL _oot_board)
  # FIXTURES_REQUIRED is named explicitly: without it ctest -j runs cmake --install on the
  # build dir concurrently with kickos_build and fails about one run in ten.
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(
      NAME    oot_export
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_oot_export.sh"
              "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
              "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
    kickos_host_gate(oot_export TIMEOUT 300)
    set_tests_properties(oot_export PROPERTIES FIXTURES_REQUIRED kickos_build)
  else()
    # The HOST readelf, not this board's: it reads every machine the fleet targets, and the
    # xtensa and rx toolchains ship none for CMAKE_READELF to find.
    add_test(
      NAME    oot_export_mcu
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_oot_export_mcu.sh"
              "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
              "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
    kickos_host_gate(oot_export_mcu TIMEOUT 300)
    set_tests_properties(oot_export_mcu PROPERTIES FIXTURES_REQUIRED kickos_build)
  endif()
endif()

if(KICKOS_HAVE_MPU AND KICKOS_ARCH STREQUAL "armv7m")
  add_test(
    NAME    kernel_ctor_placement
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_kernel_ctor_placement.sh"
            "$<TARGET_FILE:selftest>" "${CMAKE_NM}" "${CMAKE_OBJCOPY}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            "$<TARGET_FILE:kickos_lib>")
  kickos_host_gate(kernel_ctor_placement TIMEOUT 60)
endif()

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
# The CLASS BACKEND this image actually linked, which is outside the rescan group by
# construction (it must precede it) and so is in none of the lists above. Without it the
# shadowing legs below cannot see the very definition they exist to protect: MEASURED, a
# selftest handed the SPI proxy beside its own mock passed the gate green. Read off the app
# target rather than off the selection, so an image that declares no class adds nothing and
# the gate does not inventory a definition this link never had to choose from.
get_target_property(_class_backends selftest KICKOS_APP_CLASS_BACKENDS)
if(_class_backends)
  foreach(_cb ${_class_backends})
    list(APPEND _seam_archives "$<TARGET_FILE:${_cb}>")
  endforeach()
endif()
list(APPEND _seam_archives "$<TARGET_OBJECTS:selftest>")
add_test(
  NAME    seam_defaults
  COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_seam_defaults.sh"
          "${CMAKE_NM}" "${CMAKE_READELF}"
          "${_selftest_elf}" "${_selftest_map}"
          "${PROJECT_SOURCE_DIR}/tests/static/weak_allowlist.txt"
          ${_seam_archives})
kickos_host_gate(seam_defaults)

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
kickos_host_gate(class_backend)

# RISC-V small-data guard: the KickOS libs (built -msmall-data-limit=0) must emit ZERO
# .sdata/.sbss, so gp anchors the app's window and nothing of the kernel's. NECESSARY AND NOT
# SUFFICIENT on the translating board, which is why riscv_kernel_gp is registered beside it: gp
# addressing is MADE BY THE LINKER, so an archive with no .sdata at all can still end up with
# gp-relative kernel accesses in the linked image.
if((KICKOS_HAVE_MPU AND KICKOS_ARCH STREQUAL "rv32imac")
   OR (KICKOS_HAVE_ASPACE AND KICKOS_ARCH STREQUAL "rv64imac"))
  add_test(
    NAME    riscv_no_smalldata
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_riscv_no_smalldata.sh"
            "${CMAKE_OBJDUMP}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            "$<TARGET_FILE:kickos_lib>")
  kickos_host_gate(riscv_no_smalldata TIMEOUT 60)
endif()

# The three rv64 image readers below take THREE images, so a reference the optimiser only emits
# in one of them is still in the corpus.
if(KICKOS_HAVE_ASPACE AND KICKOS_ARCH STREQUAL "rv64imac")
  # The LINKED image, which is the only place gp addressing exists: the linker MAKES a
  # gp-relative access out of an ordinary upper/lower pair whose target lands inside
  # gp +/- 0x800, so a kernel reference to an app-half symbol links silently through the app's
  # own anchor.
  add_test(
    NAME    riscv_kernel_gp
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_riscv_kernel_gp.sh"
            "${CMAKE_OBJDUMP}" ".text"
            "$<TARGET_FILE:selftest>"
            "$<TARGET_FILE:hello>"
            "$<TARGET_FILE:cxxtest>")
  kickos_host_gate(riscv_kernel_gp)

  # The same hazard in its larger form: the app window is at 0x40000000, inside medlow's
  # absolute reach, so the linker relaxes such a reference to lui+addi and the link succeeds
  # whether or not gp is involved. This one reads the kernel archives' relocations, which name
  # the symbol an instruction operand resolves to whatever the linker did to the encoding.
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
  kickos_host_gate(riscv_kernel_apphalf)

  # And the kernel window's own leaves, out of the same three images. The boundary they are held
  # against is the image's own section table, so a layout that moved a fetched or a stored
  # section across it fails here rather than at the first access.
  add_test(
    NAME    riscv_kernel_wx
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/riscv_kernel_wx.py"
            "${CMAKE_READELF}"
            "$<TARGET_FILE:selftest>"
            "$<TARGET_FILE:hello>"
            "$<TARGET_FILE:cxxtest>")
  kickos_host_gate(riscv_kernel_wx)
endif()

# App-window leak guard for the inverted .appdata scheme: the enforcing linker scripts name the
# CLOSED privileged set and the app sections catch everything else, so ONE renamed selector drops
# a whole archive into a window domain.cc grants to every unprivileged thread, with the script's
# own ASSERT(_ebss > _sbss) still true.
#
# The window bounds and the privileged set both differ by board shape, so both are arguments.
# MPU boards carve one writable window and select kernel/arch/chip/lib kernel-side.
if(KICKOS_HAVE_MPU AND NOT KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    appdata_no_kernel
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_appdata_no_kernel.sh"
            "${CMAKE_NM}"
            "$<TARGET_FILE:selftest>"
            "${_selftest_map}"
            "__kickos_appdata_start:__kickos_appdata_end"
            "--"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            "$<TARGET_FILE:kickos_lib>")
  kickos_host_gate(appdata_no_kernel TIMEOUT 60)
endif()

# Split-image boards separate writable app data and user executable code.
# Keep kernel, arch, and chip archives outside both; libkickos_lib is app code.
# Exclude x86-64: its PE linker script defines empty app windows, so this
# gate cannot check placement there.
if(KICKOS_HAVE_ASPACE AND NOT KICKOS_ARCH STREQUAL "x86_64")
  add_test(
    NAME    appdata_no_kernel
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_appdata_no_kernel.sh"
            "${CMAKE_NM}"
            "$<TARGET_FILE:selftest>"
            "${_selftest_map}"
            "__kickos_app_sram_start:__kickos_app_sram_end"
            "__kickos_app_rom_start:__kickos_app_rom_end"
            "--"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>")
  kickos_host_gate(appdata_no_kernel TIMEOUT 60)
endif()
