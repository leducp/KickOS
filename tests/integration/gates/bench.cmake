# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding `bench`, which the parent builds wherever KICKOS_BENCH is on.
#
# EVERY set_tests_properties HERE IS GUARDED BY if(TEST): kickos_add_qemu_test registers
# nothing for a board with no emulator, and naming a test that does not exist is a configure
# ERROR rather than a missing property, so an unguarded one refuses a silicon board the
# moment KICKOS_BENCH is turned on above one kernel core.

if(NOT TARGET bench)
  return()
endif()

# The microbench accumulators are one row per kernel core and the two reports aggregate the
# rows. Keyed on the KERNEL-core count and not the machine's: under AMP one kernel schedules
# one core, so there is one row and no spread to read.
#
# Serial: the sweep the phase table covers is a quarter of a million round trips on four
# emulated cores, and the gate's poll bound is wall clock, so peers competing for host CPU
# turn a correct image red.
if(KICKOS_KERNEL_CORES GREATER 1)
  kickos_add_qemu_test(NAME ${_tag}_bench_percore TARGET bench
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_percore.sh"
    ARGS ${KICKOS_KERNEL_CORES}
    TIMEOUT 240)
  if(TEST ${_tag}_bench_percore)
    set_tests_properties(${_tag}_bench_percore PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

# THE WHOLE PHASE TABLE ON THE WIRE, counted against the row count the header declares. The
# console refuses a line it cannot take whole, so a table longer than the ring loses rows one
# at a time and the report reads as a shorter build. Registered at EVERY core count: the
# printer is the same at one, and so is the ring it outruns.
#
# Serial for the same reason as the gates around it: the sweep ahead of the table is a quarter
# of a million round trips and the poll bound is wall clock.
kickos_add_qemu_test(NAME ${_tag}_bench_phase_table TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_phase_table.sh"
  TIMEOUT 240)
if(TEST ${_tag}_bench_phase_table)
  set_tests_properties(${_tag}_bench_phase_table PROPERTIES RUN_SERIAL TRUE)
endif()

# The switch bracket's stamp cell, read out of the linked bench image. Keyed on the KERNEL-core
# count: at one core nothing else stamps, and under AMP one kernel schedules one core.
# It runs no image, so it carries the host label.
#
# Structural rather than a bound on the samples: the kernel lock spans the whole bracket, so
# two cores never sit inside it at once and a shared cell reads back clean at runtime. The
# addressing is what the claim rests on.
if(KICKOS_KERNEL_CORES GREATER 1)
  add_test(NAME ${_tag}_bench_stamp_percore
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_bench_stamp_percore.sh"
            "$<TARGET_FILE:bench>" "${CMAKE_OBJDUMP}" "${KICKOS_ARCH}")
  kickos_host_gate(${_tag}_bench_stamp_percore)
endif()

# The end-to-end protocol's release/acquire publication, read out of the linked bench image.
# Above one kernel core only: at one core the arm, the raise and the close run there and the
# interrupt is the only interleaving, so the state is relaxed on purpose and this gate would
# refuse a correct image. It runs no image, so it carries the host label.
#
# Structural rather than a bound on the samples: TCG models no store buffer, so an image whose
# every publication is relaxed reports the same closed count, the same split and the same
# nanosecond columns as a correct one on every vehicle in this tree.
if(KICKOS_KERNEL_CORES GREATER 1)
  add_test(NAME ${_tag}_bench_e2e_publish
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_bench_e2e_publish.sh"
            "$<TARGET_FILE:bench>" "${CMAKE_OBJDUMP}" "${KICKOS_ARCH}")
  kickos_host_gate(${_tag}_bench_e2e_publish)
endif()

# The LX6 switch bracket's END stamp, read out of the linked image: the cell must be consumed
# by the read that banks a sample. There is no LX6 emulator in this tree, so no run reads that
# row at all. It runs no image, so it carries the host label.
#
# At EVERY core count: the bracket is the same body at one, which is where this board's bench
# image is built.
if(KICKOS_ARCH STREQUAL "lx6")
  add_test(NAME ${_tag}_bench_xtensa_stamp
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_bench_xtensa_stamp.sh"
            "$<TARGET_FILE:bench>" "${CMAKE_OBJDUMP}")
  kickos_host_gate(${_tag}_bench_xtensa_stamp)
endif()

# The bench cycle source's PMU programming on armv8a, read out of the linked image: PMCR_EL0
# must carry LC. QEMU returns PMCCNTR_EL0 at 64 bits whether or not the bit is set, so no run on
# any vehicle here separates the two images and the claim can only be held statically. It runs
# no image, so it carries the host label.
if(KICKOS_ARCH STREQUAL "armv8a")
  add_test(NAME ${_tag}_bench_a53_pmcr
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_bench_a53_pmcr.sh"
            "$<TARGET_FILE:bench>" "${CMAKE_OBJDUMP}" "${KICKOS_ARCH}")
  kickos_host_gate(${_tag}_bench_a53_pmcr)
endif()

# The outermost lock's HOLD and WAIT distributions, and the nesting/core probe that says the
# counts below mean what they are read as. Registered at EVERY core count: at one the WAIT slot
# does not exist and the HOLD sample is the interrupt-masked window alone, which is the figure
# M9 asks for per core.
kickos_add_qemu_test(NAME ${_tag}_bench_lock TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_lock.sh"
  ARGS ${KICKOS_KERNEL_CORES}
  TIMEOUT 300)
if(KICKOS_KERNEL_CORES GREATER 1)
  # Same reason as the gate above: the sweep is a quarter of a million round trips on four
  # emulated cores and the poll bound is wall clock.
  if(TEST ${_tag}_bench_lock)
    set_tests_properties(${_tag}_bench_lock PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

# The doorbell ROUND TRIP, and the per-core bursts that say the figures under it were raised
# from four different cores. Above one kernel core only: BD_DOORBELL is not declared at one,
# where a raise has no peer to answer it. Same serialisation reason as the two gates above.
if(KICKOS_KERNEL_CORES GREATER 1)
  kickos_add_qemu_test(NAME ${_tag}_bench_doorbell TARGET bench
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_doorbell.sh"
    ARGS ${KICKOS_KERNEL_CORES}
    TIMEOUT 300)
  if(TEST ${_tag}_bench_doorbell)
    set_tests_properties(${_tag}_bench_doorbell PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

# The IRQ distributions: the inject-to-handler row, the four masked spans, and the end-to-end
# span with its locality split. Keyed on the KERNEL-core count, which is what decides whether
# the cross-core row exists at all and therefore whether the split can be read.
kickos_add_qemu_test(NAME ${_tag}_bench_irqspan TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_irqspan.sh"
  ARGS ${KICKOS_KERNEL_CORES}
  TIMEOUT 480)
if(TEST ${_tag}_bench_irqspan)
  set_tests_properties(${_tag}_bench_irqspan PROPERTIES RUN_SERIAL TRUE)
endif()

# THE SAME GATE'S PARSER, proven on its planted reports and nothing else. Those plants run and
# finish before the capture is touched, so the arms that read a report's SHAPE need no image and
# no emulator; the entry above is what reads a real one. Registered on EVERY board, silicon
# included, because a board with no emulator registers no image gate at all and the parser half
# would otherwise be witnessed only where four harts boot. It runs no image, so it carries the
# host label.
#
# ONE SCRIPT AND TWO REGISTRATIONS, never two files: plants kept apart from the arms they prove
# drift from them, and a plant that no longer matches its arm reports green.
add_test(NAME ${_tag}_bench_irqspan_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_irqspan.sh" --controls)
kickos_host_gate(${_tag}_bench_irqspan_controls)

# KOS_SYS_BENCH's own authority and bounds, asserted by the image rather than read off a
# report: twenty-two arms, the root half being the positive control for the child refusal
# beside it.
# The count is the same at every core count: the doorbell arms expect -KOS_ENOSYS at one
# core, where that op is compiled out ahead of its own checks.
if(TARGET benchauth)
  kickos_add_qemu_test(NAME ${_tag}_benchauth TARGET benchauth
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
    ARGS benchauth 22
    TIMEOUT 240)
endif()
