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

# The two arms that need a peer running a kernel of its own. They are compiled on every AMP
# posture and decide at RUNTIME off the peer's own serviced count, so this permission covers the
# image run STANDALONE: the same binary in a merged partition finds a peer and reports ok, and
# tests/integration/check_amp_peer_arms.sh asserts they are NOT skipped there. Under one image
# the peers are this image's own cores and always answer.
#
# amp_far_reply_guard and amp_far_reply_empty both park a caller on a far port nobody answers,
# which is a port the partition names a node MORE THAN ONCE: the node's kernel binds every port
# it is named and its app receives on the first alone. That spare is a far entry for every other
# node, and a far entry this node binds itself is not one it can park on, so the arms are
# reachable on every node but the one holding the spare, at any width.
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

# amp_deferred_doorbell needs a raise that can be WITHHELD, so it needs a doorbell that keeps a
# per-core seat. The RP2350's raise is one write to SIO DOORBELL_OUT_SET naming the other core
# of the pair, addressing it from reset with no publication behind it, so arch_ipi_seat_set
# answers ARCH_IPI_SEAT_NONE, the probe refuses the scenario ahead of the publication and the
# arm skips itself by name.
#
# NAMED BY CHIP because the seat is the chip backend's. This permission covers exactly the
# parts whose backend answers ARCH_IPI_SEAT_NONE; a part whose doorbell does keep a seat runs
# the arm and must not be added here, or the permission stops being evidence.
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
# irq_kernel_line_reserved refuses a capability AND an inject over a line the arch dispatches to
# a kernel vector of its own, and its ordinary-line control runs everywhere. A controller that
# reserves NO line leaves that first leg no subject, so the arm reports PARTIAL there rather
# than an unqualified ok.
#
# The doorbell's SGI or bell is the only such line in the tree, so the postures that CAN witness
# it are those whose controller keeps one (the GIC backends, the RP2350's SIO bell) AND whose
# image has a doorbell at all. Keyed on KICKOS_NUM_CORES and NOT on the kernel-core count: the
# AMP posture drives four cores on one kernel and links the doorbell all the same.
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

# One image per board, at the arm count that image recorded, under the derived
# <board tag>_selftest name. microbit is the exception and it is the FLASH size, not the board:
# 64 KiB parts build the suite as three images, so that board runs three gates below.
if(NOT KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(TARGET selftest
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh"
    ARGS ${_selftest_arms})
  if(TEST ${_tag}_selftest)
    set_property(TEST ${_tag}_selftest APPEND PROPERTY ENVIRONMENT ${_selftest_env})
  endif()
endif()

if(KICKOS_BOARD STREQUAL "microbit")
  # The sets below are partitioned BY REGION, NOT BY NAME: an arm belongs to the part whose
  # region holds its TAP_ADD line in main.cc. check_tap_stream.sh reports a name declared in
  # the wrong part as a NOTE and not a failure, so only that rule catches one.
  #
  # Both lists are a MEASUREMENT and not slack: a listed arm that did NOT skip is only a NOTE,
  # so a stale list quietly permits a regression.
  set(_mb_skips_selftest "")
  set(_mb_partials_selftest "")
  set(_mb_skips_selftest_p2 "uart_service")
  set(_mb_partials_selftest_p2 "")
  # irq_as_event asks the arena for a 4 KiB MMIO page after the suite's threads have taken
  # their stacks from it, and on 32 KiB it no longer fits: this board's console TX ring costs
  # 256 bytes of .bss, which is what that page stood on. The arm sees the alloc fail and skips
  # itself by name. Measured: a 128-byte ring restores it, and that needs a 64-byte line
  # bound, below the fault reporter's own KDIAG_FAULT_LINE_MAX.
  set(_mb_skips_selftest_p3 "domain_share,confused_deputy,mem_self_grant,irq_as_event")
  set(_mb_partials_selftest_p3 "caller_stack,mmio_grant")
  # DERIVED from the decision above rather than restated: these three sets are literals, so a
  # permission appended to the whole-suite list reached every other board and not this one,
  # and a partitioned board's gate reported that as a failure. Region 3 holds the arm's
  # TAP_ADD line, so this is the part that carries it.
  if(_selftest_kernel_line_partial)
    set(_mb_partials_selftest_p3
        "${_mb_partials_selftest_p3},irq_kernel_line_reserved")
  endif()
  foreach(_img selftest selftest_p2 selftest_p3)
    get_target_property(_mb_arms ${_img} KICKOS_TAP_ARMS)
    kickos_add_qemu_test(TARGET ${_img}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_selftest.sh"
      ARGS ${_mb_arms})
    set_property(TEST microbit_${_img} APPEND PROPERTY ENVIRONMENT
      "EXPECT_SKIPS=${_mb_skips_${_img}}"
      "EXPECT_PARTIALS=${_mb_partials_${_img}}"
      "EXPECT_FAULTS=${KICKOS_EXPECT_FAULTS}")
  endforeach()
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

# The split-image boards carve TWO windows: the app's writable state, and the app's own
# EL0/U-mode executable half, which is the one a kernel object landing app-side turns into
# fetchable privileged code. Their scripts select kernel/arch/chip kernel-side and leave
# libkickos_lib.a app-side by design, so the privileged set here is three and not four.
#
# x86_64 is out and stays out: arch/x86/x86_64/pe_image.ld carves no app window at all and
# STATES each one as start == end, `ld -m i386pep` building no GOT so a weak-undefined window
# symbol would resolve to itself. Dropping this exclusion does not make the gate vacuous
# there, it makes it fail on the empty window, which is the loud end of that mistake.
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
