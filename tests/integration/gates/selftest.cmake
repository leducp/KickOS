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

# The selftest creates no exception, so no image of it links the exception runtime.
if(NOT KICKOS_ARCH STREQUAL "sim")
  get_property(_selftest_eh_images GLOBAL PROPERTY KICKOS_SELFTEST_IMAGES)
  foreach(_img IN LISTS _selftest_eh_images)
    get_target_property(_img_map ${_img} KICKOS_IMAGE_MAP)
    add_test(NAME ${_tag}_${_img}_no_eh_runtime
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_no_eh_runtime.sh" "${_img_map}")
    kickos_host_gate(${_tag}_${_img}_no_eh_runtime TIMEOUT 60)
  endforeach()
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
    prio_self_raise_lower prio_self_boosted
    reply_abandoned_cap call_timeout_revert call_infoless_revert call_close_reply
    call_donation call_donation_hold call_donation_slow call_donation_pending
    reply_recv_notify reply_recv_notify_park
    cap_reply_bound_fast cap_reply_bound_slow thread_slay_timeout
    mutex_owner_died_nowaiter)
  # Registered only where tasks have address spaces of their own.
  if(KICKOS_HAVE_ASPACE)
    list(APPEND KICKOS_EXPECT_SKIPS aspace_two_spaces_same_grant parked_frame_hostile)
  endif()
endif()

# The arms over the sync a system call makes outside the kernel lock, which rv64imac, having no
# cacheable kernel view of a non-cacheable frame, never makes.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_HAVE_ASPACE AND KICKOS_ARCH STREQUAL "rv64imac")
  list(APPEND KICKOS_EXPECT_SKIPS presync_retried presync_race presync_flip presync_cancel)
endif()

# The refusal arm caps its parked children at 24 (LR_PARK_CAP, selftest_aspace.cc), so a
# thread pool wider than that can reach the cap before the refusal it measures. DERIVED and not
# a posture list: qemu-arm64's benchsmp12 image and qemu-x86_64's bench and SMP images carry one.
if(KICKOS_MAX_THREADS GREATER 24)
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

# The capability slots main's table holds before any arm runs: the crossings its composition
# names, delegated from index 1 and at most one per partition port, else the reserved indices,
# and the two it holds for the run.
set(_selftest_lifetime_caps 2) # g_lock and g_done, created by main and never closed
math(EXPR _selftest_cap_seated "1 + ${KICKOS_AMP_PORT_COUNT}")
if(_selftest_cap_seated LESS KICKOS_CAP_FIRST_DYNAMIC)
  set(_selftest_cap_seated ${KICKOS_CAP_FIRST_DYNAMIC})
endif()
math(EXPR _selftest_cap_seated "${_selftest_cap_seated} + ${_selftest_lifetime_caps}")

# Every arm that asks a pool for more than this posture provisions, by what the arm itself asks
# for (tests/static/selftest_demands.py): the workers beside main, the capability slots beside the
# seated ones, and the objects of each kind main's task budget leaves beside the two semaphores
# main holds for the run.
set(_selftest_lifetime_sems 2) # g_lock and g_done
foreach(_selftest_knob KICKOS_MAX_THREADS KICKOS_CAP_TABLE_SUPPLY KICKOS_TASK_SEMAPHORE_BUDGET
        KICKOS_TASK_MUTEX_BUDGET KICKOS_TASK_ENDPOINT_BUDGET KICKOS_TASK_NOTIFY_BUDGET)
  if(NOT "${${_selftest_knob}}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR "selftest: the generated configuration states no ${_selftest_knob}")
  endif()
endforeach()
math(EXPR _selftest_workers "${KICKOS_MAX_THREADS} - 1")
math(EXPR _selftest_caps "${KICKOS_CAP_TABLE_SUPPLY} - ${_selftest_cap_seated}")
math(EXPR _selftest_sems "${KICKOS_TASK_SEMAPHORE_BUDGET} - ${_selftest_lifetime_sems}")
# The guards an arm's registration sits under, as the compiler sees them.
set(_selftest_guards --config "KICKOS_KERNEL_CORES=${KICKOS_KERNEL_CORES}")
foreach(_selftest_knob KICKOS_HAVE_ASPACE KICKOS_HAVE_MPU KICKOS_MEMORY_ENFORCED
        KICKOS_FAULT_ISOLATION KICKOS_AMP_NODE KICKOS_AMP_OWN_IMAGE)
  set(_selftest_on 0)
  if(${_selftest_knob})
    set(_selftest_on 1)
  endif()
  list(APPEND _selftest_guards --config "${_selftest_knob}=${_selftest_on}")
endforeach()
if(KICKOS_ENABLE_SELFTEST)
  list(APPEND _selftest_guards --config KICKOS_ENABLE_SELFTEST=1)
else()
  list(APPEND _selftest_guards --undefined KICKOS_ENABLE_SELFTEST)
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
  COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/static/selftest_demands.py"
          "${PROJECT_SOURCE_DIR}/user/apps/common/selftest" ${_selftest_guards} --supply
          "workers=${_selftest_workers}" "caps=${_selftest_caps}" "sems=${_selftest_sems}"
          "mutexes=${KICKOS_TASK_MUTEX_BUDGET}" "endpoints=${KICKOS_TASK_ENDPOINT_BUDGET}"
          "notifies=${KICKOS_TASK_NOTIFY_BUDGET}"
  OUTPUT_VARIABLE _selftest_short
  ERROR_VARIABLE _selftest_demands_err
  RESULT_VARIABLE _selftest_demands_rc)
if(NOT _selftest_demands_rc EQUAL 0)
  message(FATAL_ERROR "selftest: the arms' demands could not be read: ${_selftest_demands_err}")
endif()
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${PROJECT_SOURCE_DIR}/tests/static/selftest_demands.py")
file(GLOB _selftest_sources "${PROJECT_SOURCE_DIR}/user/apps/common/selftest/*.cc")
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${_selftest_sources})
string(REPLACE "\n" ";" _selftest_short "${_selftest_short}")
list(APPEND KICKOS_EXPECT_SKIPS ${_selftest_short})
# console_publish_narrow narrows what console_publish_handout published.
if("console_publish_handout" IN_LIST KICKOS_EXPECT_SKIPS)
  list(APPEND KICKOS_EXPECT_SKIPS console_publish_narrow)
endif()

# irq_as_event's 4 KiB page and caller_stack's accepted stack come from the arena the image
# leaves, which on a part of 32 KiB of RAM or less spares neither beside the suite's stacks.
set(_selftest_arena_partials "")
if(KICKOS_ENABLE_SELFTEST AND DEFINED KICKOS_CHIP_LINK_RAM_LENGTH)
  math(EXPR _selftest_ram "${KICKOS_CHIP_LINK_RAM_LENGTH}")
  if(_selftest_ram LESS_EQUAL 32768)
    list(APPEND KICKOS_EXPECT_SKIPS irq_as_event)
    set(_selftest_arena_partials caller_stack)
  endif()
endif()

# uart_service where the app pins it out of the arena (user/apps/common/selftest).
get_target_property(_selftest_defs selftest COMPILE_DEFINITIONS)
if("KICKOS_SELFTEST_NO_UART_SERVICE=1" IN_LIST _selftest_defs)
  list(APPEND KICKOS_EXPECT_SKIPS uart_service)
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1 AND NOT KICKOS_LIBC_REENT)
  list(APPEND KICKOS_EXPECT_SKIPS reent_per_thread_cores)
endif()

# Peer-dependent arms skip when an AMP image runs alone; the merged partition gate requires them
# to run when a peer is available.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  list(APPEND KICKOS_EXPECT_SKIPS amp_far_call amp_share_crossing)
  # The far-reply arms park a call on a far port nobody receives on: with an echo crossing any
  # far service port, without one the second, a node serving only its first. They skip where
  # this node's far entries hold none.
  set(_selftest_far_service 0)
  set(_selftest_far_echo 0)
  string(REPLACE "," ";" _selftest_amp_entries "${KICKOS_AMP_PORTS}")
  foreach(_selftest_entry IN LISTS _selftest_amp_entries)
    string(REPLACE ":" ";" _selftest_entry "${_selftest_entry}")
    list(GET _selftest_entry 0 _selftest_entry_node)
    list(GET _selftest_entry 1 _selftest_entry_port)
    if(NOT _selftest_entry_node EQUAL KICKOS_AMP_NODE_ID)
      if(_selftest_entry_port EQUAL 0)
        set(_selftest_far_echo 1)
      else()
        math(EXPR _selftest_far_service "${_selftest_far_service} + 1")
      endif()
    endif()
  endforeach()
  set(_selftest_far_unanswered 2)
  if(_selftest_far_echo)
    set(_selftest_far_unanswered 1)
  endif()
  if(_selftest_far_service LESS _selftest_far_unanswered)
    list(APPEND KICKOS_EXPECT_SKIPS amp_far_reply_guard amp_far_reply_empty)
  endif()
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

# The arms that skip on a doorbell keeping no seat, where the chip file states none.
include("${PROJECT_SOURCE_DIR}/tests/integration/selftest_partials.cmake")
set(_selftest_seat ON)
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE)
  if(NOT KICKOS_CHIP_DOORBELL_SEAT MATCHES "^(ON|OFF)$")
    message(FATAL_ERROR "selftest: the generated chip.cmake states no KICKOS_CHIP_DOORBELL_SEAT")
  endif()
  set(_selftest_seat ${KICKOS_CHIP_DOORBELL_SEAT})
endif()
_selftest_seat_skips("${PROJECT_SOURCE_DIR}/user/apps/common/selftest/main.cc" ${_selftest_seat}
                     _selftest_seat_skips)
list(APPEND KICKOS_EXPECT_SKIPS ${_selftest_seat_skips})

# The two console_publish arms publish the console themselves, which they refuse to do in an
# image whose composition names a console driver as `stdout`.
set(_selftest_driver_skips console_publish_handout console_publish_narrow)

# Every name a permission set states must be an arm main.cc registers, or the set permits
# nothing and reads as if it did.
function(_selftest_names_known what)
  get_property(_known GLOBAL PROPERTY KICKOS_SELFTEST_ALL_ARM_NAMES)
  foreach(_name IN LISTS ARGN)
    if(NOT _name IN_LIST _known)
      message(FATAL_ERROR
        "selftest: ${what} names ${_name}, which no TAP_ADD line in "
        "user/apps/common/selftest/main.cc registers")
    endif()
  endforeach()
endfunction()

_selftest_starved_arms("${PROJECT_SOURCE_DIR}/user/apps/common/selftest/main.cc" _selftest_starved)

# <names> cut to the members of <keep>, once each, comma-joined into <out>.
function(_selftest_cut names keep out)
  set(_cut "")
  foreach(_name IN LISTS names)
    if(_name IN_LIST keep AND NOT _name IN_LIST _cut)
      list(APPEND _cut ${_name})
    endif()
  endforeach()
  list(JOIN _cut "," _cut)
  set(${out} "${_cut}" PARENT_SCOPE)
endfunction()

# <img>'s skip and partial sets: <skips> and <partials> cut to the arms its own regions
# register, with the image's console taken into account. The cut is by name, off the same
# region bounds that split the suite, so moving an arm across a boundary moves its permission
# with it.
function(_selftest_image_sets img skips partials out_skips out_partials)
  get_target_property(_console ${img} KICKOS_SELFTEST_CONSOLE)
  get_target_property(_names ${img} KICKOS_SELFTEST_ARM_NAMES)
  if(NOT _names)
    message(FATAL_ERROR "selftest: the image ${img} records no KICKOS_SELFTEST_ARM_NAMES")
  endif()
  set(_skips ${skips})
  if(NOT _console STREQUAL "kernel")
    list(APPEND _skips ${_selftest_driver_skips})
  endif()
  set(_partials ${partials})
  get_target_property(_system ${img} KICKOS_APP_SYSTEM)
  get_property(_composition GLOBAL PROPERTY KICKOS_APP_SYSTEM_${_system})
  list(GET _composition 0 _first)
  if(_first STREQUAL "PARTITION")
    math(EXPR _at "${KICKOS_AMP_NODE_ID} + 1")
    list(GET _composition ${_at} _composition)
  endif()
  if(NOT EXISTS "${_composition}")
    message(FATAL_ERROR "selftest: the image ${img} names no composition of its own (${_composition})")
  endif()
  _selftest_driver_tasks("${_composition}" _driver_tasks)
  _selftest_derived_partials("${_selftest_starved}" "${_skips}" ${_driver_tasks} ${KICKOS_MAX_TASKS}
                             _derived)
  list(APPEND _partials ${_derived})
  # caller_stack_overlap runs on caller_stack's block, so it skips wherever that block is short.
  if("caller_stack" IN_LIST _partials)
    list(APPEND _skips caller_stack_overlap)
  endif()
  _selftest_cut("${_skips}" "${_names}" _skips)
  _selftest_cut("${_partials}" "${_names}" _partials)
  set(${out_skips} "${_skips}" PARENT_SCOPE)
  set(${out_partials} "${_partials}" PARENT_SCOPE)
endfunction()

# The same for the arms that report PARTIAL. A PARTIAL reports `ok`, so neither the plan/case
# reconciliation nor the skip bookkeeping can see one and this by-name set is the only thing
# that can. It is NOT derivable from the arm count: the conditions below name the postures.
set(KICKOS_EXPECT_PARTIALS ${_selftest_arena_partials})
# Above one kernel core, four tier-1 IRQ arms carry a claim about an event that must NOT happen
# (a service, a redelivery or a wake), and a non-event raises nothing to order a later read
# after, so there is no closed interval to read the result in. The one-core fleet checks them.
if(KICKOS_KERNEL_CORES GREATER 1)
  list(APPEND KICKOS_EXPECT_PARTIALS irq_spurious irq_mask_coalesce irq_discard
                               irq_stale_register)
endif()
# irq_kernel_line_reserved tests both claim and injection rejection for a
# kernel-reserved line. Report PARTIAL when none exists. GIC and RP2350 have
# such a line when the image uses doorbells. Test KICKOS_NUM_CORES because
# AMP images also need doorbells even with one kernel core.
if(KICKOS_ENABLE_SELFTEST
   AND NOT ((KICKOS_ARCH STREQUAL "armv8a" OR KICKOS_CHIP STREQUAL "rp2350")
            AND (KICKOS_NUM_CORES GREATER 1 OR KICKOS_AMP_NODE)))
  list(APPEND KICKOS_EXPECT_PARTIALS irq_kernel_line_reserved)
endif()
# periph_reg_write_unheld on every backend whose peripheral model cannot witness the refusal.
if(KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "armv8a"
   OR KICKOS_ARCH STREQUAL "rv64imac" OR KICKOS_ARCH STREQUAL "x86_64")
  list(APPEND KICKOS_EXPECT_PARTIALS periph_reg_write_unheld)
endif()
# vector_fault_contained's SIMD half, where the emulator flags an unmasked SIMD exception and
# raises no #XM, which QEMU's TCG does.
if(KICKOS_ARCH STREQUAL "x86_64")
  list(APPEND KICKOS_EXPECT_PARTIALS vector_fault_contained)
endif()
# cap_chunk_span needs main's table, a child's, WIDER than the chunk granule
# (KICKOS_CAP_CHUNK_TARGET) to reach a segmented index. A child's width is
# KICKOS_MAX_SPAWN_GRANTS + 1.
math(EXPR _selftest_child_floor "${KICKOS_MAX_SPAWN_GRANTS} + 1")
if(_selftest_child_floor LESS_EQUAL KICKOS_CAP_CHUNK_TARGET)
  list(APPEND KICKOS_EXPECT_PARTIALS cap_chunk_span)
endif()
# It also needs a free slot BELOW the granule, and main's own creates start above everything
# seated before its first create.
if(NOT _selftest_cap_seated LESS KICKOS_CAP_CHUNK_TARGET)
  list(APPEND KICKOS_EXPECT_PARTIALS cap_chunk_span)
endif()
# amp_mint_reply_port holds the peer's echo port as a port the partition does not name. The
# peer is amp_round_peer's: node 1 for node 0, node 0 for every other.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE)
  set(_selftest_amp_peer 0)
  if(KICKOS_AMP_NODE_ID EQUAL 0)
    set(_selftest_amp_peer 1)
  endif()
  if("${KICKOS_AMP_PORTS}" MATCHES "(^|,)${_selftest_amp_peer}:0(,|$)")
    list(APPEND KICKOS_EXPECT_PARTIALS amp_mint_reply_port)
  endif()
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  # The reserved-overlap matrix needs at least one arch_reserved_blocks entry.
  if(NOT KICKOS_CHIP_RESERVED_BLOCKS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "selftest: the generated chip.cmake states no KICKOS_CHIP_RESERVED_BLOCKS")
  endif()
  if(KICKOS_CHIP_RESERVED_BLOCKS EQUAL 0)
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
  list(APPEND KICKOS_EXPECT_FAULTS fvic kvic)
  # window_addr's sibling, reading the window its holder took with it at exit.
  if(KICKOS_CHIP STREQUAL "virt_arm64" OR KICKOS_CHIP STREQUAL "virt_rv64"
     OR KICKOS_CHIP STREQUAL "q35")
    list(APPEND KICKOS_EXPECT_FAULTS was)
  endif()
  # port_window's COM2 holder reaching the CMOS data port, and its index writer, and
  # vector_fault_contained's two victims, an unmasked x87 and an unmasked SIMD divide by zero.
  if(KICKOS_ARCH STREQUAL "x86_64")
    list(APPEND KICKOS_EXPECT_FAULTS pwb pwi vfmf vfxm)
  endif()
endif()
# window_memory_ro's child, writing through its read-only window, and the task_exit arms'
# sibling, writing a reservation of main's.
if(KICKOS_MEMORY_ENFORCED AND KICKOS_FAULT_ISOLATION)
  list(APPEND KICKOS_EXPECT_FAULTS wro txf)
endif()
# task_exit_driver_trap's thread, trapping as a failing packaged driver thread does.
if(KICKOS_FAULT_ISOLATION)
  list(APPEND KICKOS_EXPECT_FAULTS txtrap)
endif()

# Comma-separated, never semicolons: ENVIRONMENT is itself a CMake list, so a raw list
# deref would split the value into further bogus environment entries.
list(REMOVE_DUPLICATES KICKOS_EXPECT_SKIPS)
set(_selftest_skips_list ${KICKOS_EXPECT_SKIPS})
set(_selftest_partials_list ${KICKOS_EXPECT_PARTIALS})
_selftest_names_known("the expected-skip set" ${_selftest_skips_list} ${_selftest_driver_skips})
_selftest_names_known("the expected-partial set" ${_selftest_partials_list})
list(JOIN KICKOS_EXPECT_SKIPS "," KICKOS_EXPECT_SKIPS)
list(JOIN KICKOS_EXPECT_PARTIALS "," KICKOS_EXPECT_PARTIALS)
list(JOIN KICKOS_EXPECT_FAULTS "," KICKOS_EXPECT_FAULTS)
set(_selftest_env
  "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}" "EXPECT_PARTIALS=${KICKOS_EXPECT_PARTIALS}"
  "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")

# The same expectations where a capture off silicon can read them: the bench runs
# tests/integration/check_tap_stream.sh with the arguments this file would have given it. The arm
# count comes off each image target and the three sets are the ones handed to the entries below.
# One row per image, `|` separated because the sets are comma-joined and any of them may be empty.
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
  _selftest_image_sets(${_mf_img} "${_selftest_skips_list}" "${_selftest_partials_list}"
                      _mf_skips _mf_partials)
  string(APPEND _selftest_manifest "${_mf_img}|${_mf_arms}|${_mf_skips}|"
                                   "${_mf_partials}|${KICKOS_EXPECT_FAULTS}\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/kickos-selftest-manifest.txt" "${_selftest_manifest}")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME selftest
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_selftest.sh" "${_selftest_elf}"
            ${_selftest_arms})
  kickos_boot_timeout(selftest "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_selftest.sh")
  set_property(TEST selftest APPEND PROPERTY ENVIRONMENT ${_selftest_env})

  # The sim's only coverage of the published console route.
  get_target_property(_simcon_arms selftest_simcon KICKOS_TAP_ARMS)
  _selftest_image_sets(selftest_simcon "${_selftest_skips_list}" "${_selftest_partials_list}"
                       _simcon_skips _simcon_partials)
  add_test(
    NAME    sim_published_console
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_published.sh"
            "$<TARGET_FILE:selftest_simcon>" ${_simcon_arms})
  set_tests_properties(sim_published_console PROPERTIES TIMEOUT 60)
  set_property(TEST sim_published_console APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=${_simcon_skips}" "EXPECT_PARTIALS=${_simcon_partials}"
    "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")

endif()

# Register each board's self-test image and expected arm count. A split board runs one image
# per part, each with its own gate.
get_property(_selftest_parts GLOBAL PROPERTY KICKOS_SELFTEST_PARTS)
if(_selftest_parts EQUAL 1)
  kickos_add_qemu_test(TARGET selftest
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh"
    ARGS ${_selftest_arms})
  if(TEST ${_tag}_selftest)
    set_property(TEST ${_tag}_selftest APPEND PROPERTY ENVIRONMENT ${_selftest_env})
  endif()
endif()
get_target_property(_selftest_rebased selftest KICKOS_REBASED_IMAGE_FILE)
if(_selftest_rebased AND TEST ${_tag}_selftest)
  kickos_qemu_machine("${KICKOS_BOARD}" _rb_env _rb_machine)
  add_test(NAME ${_tag}_selftest_rebased
    COMMAND "${CMAKE_COMMAND}" -E env ${_rb_env} QEMU_MACHINE=${_rb_machine} ${_selftest_env}
            "${PROJECT_SOURCE_DIR}/tests/integration/check_x86_64_rebased.sh"
            "${_selftest_rebased}" ${_selftest_arms})
  set_tests_properties(${_tag}_selftest_rebased PROPERTIES SKIP_RETURN_CODE 77)
  kickos_boot_timeout(${_tag}_selftest_rebased
    "${PROJECT_SOURCE_DIR}/tests/integration/check_x86_64_rebased.sh")
endif()

if(_selftest_parts GREATER 1)
  get_property(_selftest_images GLOBAL PROPERTY KICKOS_SELFTEST_IMAGES)
  foreach(_img IN LISTS _selftest_images)
    get_target_property(_mb_arms ${_img} KICKOS_TAP_ARMS)
    _selftest_image_sets(${_img} "${_selftest_skips_list}" "${_selftest_partials_list}"
                         _mb_img_skips _mb_img_partials)
    kickos_add_qemu_test(TARGET ${_img}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh"
      ARGS ${_mb_arms})
    if(NOT TEST ${_tag}_${_img})
      continue()
    endif()
    set_property(TEST ${_tag}_${_img} APPEND PROPERTY ENVIRONMENT
      "EXPECT_SKIPS=${_mb_img_skips}"
      "EXPECT_PARTIALS=${_mb_img_partials}"
      "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
  endforeach()
endif()

# The out-of-tree package gate, on ONE BOARD PER KICKOS_ARCH. Registered on two of the
# seventy-one presets it leaves every arch-private installed header unexamined.
#
# The map is data (tests/integration/oot_arch_boards.txt). Every configure holds its own arch's
# row, and CI configures every arch, so a new arch cannot quietly get no row.
set(_oot_map "${PROJECT_SOURCE_DIR}/tests/integration/oot_arch_boards.txt")
# The rows match a literal space, never [ \t]: CMake's regex engine has no tab escape, so the
# class would silently reduce to "space or the letter t". A tab is refused outright instead.
file(READ "${_oot_map}" _oot_text)
string(FIND "${_oot_text}" "\t" _oot_tab)
if(NOT _oot_tab EQUAL -1)
  message(FATAL_ERROR "${_oot_map} holds a tab; its columns are separated by spaces")
endif()
file(STRINGS "${_oot_map}" _oot_rows REGEX "^[^#]")
file(GLOB _oot_descs "${PROJECT_SOURCE_DIR}/boards/*/board.cmake")
set(_oot_arches "")
foreach(_oot_desc IN LISTS _oot_descs)
  file(STRINGS "${_oot_desc}" _oot_states REGEX "^set\\(KICKOS_ARCH +\"[A-Za-z0-9_]+\"\\)")
  string(REGEX REPLACE "^set\\(KICKOS_ARCH +\"([A-Za-z0-9_]+)\"\\)$" "\\1" _oot_states
         "${_oot_states}")
  list(APPEND _oot_arches ${_oot_states})
endforeach()
set(_oot_board "")
set(_oot_own 0)
foreach(_oot_row IN LISTS _oot_rows)
  if(NOT _oot_row MATCHES "^(covers +[A-Za-z0-9_]+ +[A-Za-z0-9_-]+|declines +[A-Za-z0-9_]+ +[^ ].*)$")
    message(FATAL_ERROR "${_oot_map}: '${_oot_row}' is neither `covers <arch> <board>` nor "
                        "`declines <arch> <reason>`")
  endif()
  string(REGEX MATCH "^([a-z]+) +([A-Za-z0-9_]+) +(.*)$" _oot_m "${_oot_row}")
  if(NOT CMAKE_MATCH_2 IN_LIST _oot_arches)
    message(FATAL_ERROR "${_oot_map}: '${_oot_row}' names an arch no boards/*/board.cmake states, "
                        "so no configure ever reads it")
  endif()
  if(CMAKE_MATCH_2 STREQUAL KICKOS_ARCH)
    math(EXPR _oot_own "${_oot_own} + 1")
    if(CMAKE_MATCH_1 STREQUAL "covers")
      set(_oot_board "${CMAKE_MATCH_3}")
    endif()
  endif()
endforeach()
if(NOT _oot_own EQUAL 1)
  message(FATAL_ERROR "${_oot_map} holds ${_oot_own} rows for ${KICKOS_ARCH}, and every arch a "
                      "board states needs exactly one: covers, with the board whose preset runs "
                      "the out-of-tree package gate, or declines, with the reason no package can "
                      "be consumed on it yet")
endif()
if(_oot_board)
  set(_oot_desc "${PROJECT_SOURCE_DIR}/boards/${_oot_board}/board.cmake")
  if(NOT EXISTS "${_oot_desc}")
    message(FATAL_ERROR "${_oot_map} covers ${KICKOS_ARCH} with board ${_oot_board}, which has no "
                        "${_oot_desc}")
  endif()
  file(STRINGS "${_oot_desc}" _oot_states REGEX "^set\\(KICKOS_ARCH +\"${KICKOS_ARCH}\"\\)")
  if(_oot_states STREQUAL "")
    message(FATAL_ERROR "${_oot_map} covers ${KICKOS_ARCH} with board ${_oot_board}, whose "
                        "${_oot_desc} states another arch")
  endif()
  file(GLOB _oot_presets "${PROJECT_SOURCE_DIR}/CMakePresets.json"
                         "${PROJECT_SOURCE_DIR}/cmake/presets/*.json")
  set(_oot_sets "")
  foreach(_oot_preset IN LISTS _oot_presets)
    file(STRINGS "${_oot_preset}" _oot_set REGEX "\"KICKOS_BOARD\": *\"${_oot_board}\"")
    list(APPEND _oot_sets ${_oot_set})
  endforeach()
  if(NOT _oot_sets)
    message(FATAL_ERROR "${_oot_map} covers ${KICKOS_ARCH} with board ${_oot_board}, which no "
                        "configure preset sets as KICKOS_BOARD, so the gate runs on none")
  endif()
endif()

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
    kickos_qemu_machine("${KICKOS_BOARD}" _oot_qemu_env _oot_qemu_machine)
    if(NOT _oot_qemu_machine STREQUAL "")
      add_test(
        NAME    ${_tag}_oot_mcu_app
        COMMAND "${CMAKE_COMMAND}" -E env ${_oot_qemu_env} QEMU_MACHINE=${_oot_qemu_machine}
                "${PROJECT_SOURCE_DIR}/tests/integration/check_oot_mcu_run.sh"
                "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
      set_tests_properties(${_tag}_oot_mcu_app PROPERTIES SKIP_RETURN_CODE 77
                                                          FIXTURES_REQUIRED kickos_build)
      kickos_boot_timeout(${_tag}_oot_mcu_app
        "${PROJECT_SOURCE_DIR}/tests/integration/check_oot_mcu_run.sh" WORK 300)
    endif()
  endif()
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
if(TARGET kickos_string)
  list(APPEND _seam_archives "$<TARGET_OBJECTS:kickos_string>")
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

# The rv64 reader below takes THREE images, so a reference the optimiser only emits in one of
# them is still in the corpus.
if(KICKOS_HAVE_ASPACE AND KICKOS_ARCH STREQUAL "rv64imac")
  # The app window is at 0x40000000, inside medlow's absolute reach, so ld rewrites a kernel
  # reference to it into lui+addi and the link succeeds, relaxation or not. This reads the
  # kernel archives' relocations, which name the symbol an instruction operand resolves to
  # whatever the linker did to the encoding.
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
  # Three images, so a reference the optimiser emits in one alone is still read.
  foreach(_img IN ITEMS hello selftest cxxtest)
    kickos_image_rule(rv64_wx ${_img})
    kickos_image_rule(rv64_gp ${_img})
  endforeach()
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ARCH STREQUAL "armv7m")
  kickos_image_rule(ctor selftest)
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
# On x86_64 the boot and landing objects are the kernel's too, linked as objects rather than
# archived.
if(KICKOS_HAVE_ASPACE)
  set(_appdata_image "$<TARGET_FILE:selftest>")
  set(_appdata_map "${_selftest_map}")
  set(_appdata_kernel_objects "")
  if(KICKOS_ARCH STREQUAL "x86_64")
    set(_appdata_kernel_objects $<TARGET_OBJECTS:kickos_x86_64_landed_kernel>)
    if(KICKOS_KERNEL_CORES GREATER 1)
      list(APPEND _appdata_kernel_objects $<TARGET_OBJECTS:kickos_x86_64_boot_ap>)
    else()
      list(APPEND _appdata_kernel_objects $<TARGET_OBJECTS:kickos_x86_64_boot>)
    endif()
  endif()
  add_test(
    NAME    appdata_no_kernel
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_appdata_no_kernel.sh"
            "${CMAKE_NM}"
            "${_appdata_image}"
            "${_appdata_map}"
            "__kickos_app_sram_start:__kickos_app_sram_end"
            "__kickos_app_rom_start:__kickos_app_rom_end"
            "--"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            ${_appdata_kernel_objects}
    COMMAND_EXPAND_LISTS)
  kickos_host_gate(appdata_no_kernel TIMEOUT 60)
endif()
