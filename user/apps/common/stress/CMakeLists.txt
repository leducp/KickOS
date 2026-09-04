# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_application(stress
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# Endurance soak: repeat the conservation round forever (heartbeat per round, freeze
# on the first FAIL). OFF keeps the single-round CTest-gate behaviour byte-identical.
option(KICKOS_STRESS_FOREVER "stress: loop the conservation round forever (soak)" OFF)
if(KICKOS_STRESS_FOREVER)
  target_compile_definitions(stress PRIVATE KICKOS_STRESS_FOREVER)
endif()

# Bounded, self-verifying concurrency stress: exercises scheduler + semaphores +
# the tickless timer under many mixed-priority threads, then asserts conservation.
# The sim has NO virtual clock: arch_clock_now reads CLOCK_MONOTONIC and the one-shot is a
# real timer_create delivering SIGALRM, so this gate is wall-clock timed like the MCU runs
# and its conservation counts, not its timings, are what it asserts. On MCUs it is a
# manual soak, run per board.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_stress COMMAND "$<TARGET_FILE:stress>")
  # KICKOS_PANIC_REGEX, not a lowercase "panic|unreachable": CTest's
  # FAIL_REGULAR_EXPRESSION is case-sensitive, and the kernel prints "KERNEL PANIC:",
  # so the old pattern matched only KICKOS_UNREACHABLE and let any panic AFTER the
  # pass marker ship green.
  set_tests_properties(sim_stress PROPERTIES
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "STRESS PASS"
    FAIL_REGULAR_EXPRESSION "STRESS FAIL;${KICKOS_PANIC_REGEX}")
endif()

# Threads on every core, on THIS image and not the smallest one that boots the chip: the soak
# holds more runnable threads than the machine has cores for its whole run, so a core running
# no thread is one the scheduler left idle beside a ready thread. An app whose threads hand off
# one at a time offers a single runnable thread, and which core takes it is a race the host
# decides; under saturation it is decided the same way for the whole run, so no bound recovers
# it.
#
# The gate stops the run itself, as soon as QEMU's execution log attributes the trap stub to
# every vCPU, and it is that log rather than anything the image printed that carries the
# verdict. The liveness pattern is the app's own FIRST line, a control that the app half ran at
# all. CMAKE_NM travels with it: the gate reads the trap stub's address out of the image
# instead of carrying a layout. TIMEOUT covers the gate's own sampling bound.
#
# THE CORE COUNT STAYS THE MACHINE'S, and the isolated mask travels beside it as its own
# argument: stress names no core, and an isolated core is held out of the default core set that
# gives it, so the gate asserts a thread on every other core AND no thread at all on that one.
# It is the by-default half of the guarantee and not a claim that the core is unreachable. A
# count alone
# could not say which core the absence belongs to. The knob is absent on every posture but the
# shared model, so it carries the same zero default here that the C side does.
set(_stress_isolated 0)
if(DEFINED KICKOS_ISOLATED_CORES AND NOT KICKOS_ISOLATED_CORES STREQUAL "")
  set(_stress_isolated ${KICKOS_ISOLATED_CORES})
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64" AND KICKOS_KERNEL_CORES GREATER 1)
  kickos_add_qemu_test(NAME qemu_arm64_smp_threads TARGET stress BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_smp_threads.sh"
    ARGS ${KICKOS_KERNEL_CORES} "^stress: scheduler" "${CMAKE_NM}" armv8a
         ${_stress_isolated}
    TIMEOUT 300)
endif()

# The same gate on the second backend. Its first channel is the emulator's execution log and is
# architecture-neutral; its second reads the trap log, whose line names the hart and the cause
# where the GIC's event names the interface and the INTID.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64" AND KICKOS_KERNEL_CORES GREATER 1)
  kickos_add_qemu_test(NAME qemu_riscv64_smp_threads TARGET stress BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_smp_threads.sh"
    ARGS ${KICKOS_KERNEL_CORES} "^stress: scheduler" "${CMAKE_NM}" rv64imac
         ${_stress_isolated}
    TIMEOUT 300)
endif()
