# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# THIS FILE SELECTS. Every app below is entered only on a posture where it applies, so no app
# file asks whether it applies: an app declares its target and stops. What an app file may
# still hold is a precondition that is the APP's own and carries operator instructions with it
# (a console the kernel must own, a knob that shapes the image), never a board-level fact that
# is readable here.
#
# Two board groupings recur, and both are arch/chip facts rather than lists to maintain:
#   the four QEMU MPS2 FPGA images (an386/M4F, an505/M33, an500/M7, an385/M3) are exactly the
#     boards QEMU runs as an MPS2 machine, KICKOS_QEMU_MPS2, which the root derives from
#     kickos_qemu_machine.
#   the two emulatable arm64 boards (qemu-arm64, imx8mp-evk) are exactly the armv8a fleet,
#     KICKOS_ARCH STREQUAL "armv8a".
#
# NO board name selects an app: a name list is where an app was once run, and the next board is
# left out in silence. tests/static/check_board_predicates.py refuses one.

# _kos_chip_defines(<out> <symbol>): ON where a source of this build's chip library defines
# <symbol>, which displaces the arch/common/<symbol>_default.cc fallback.
function(_kos_chip_defines out symbol)
  set(_defined OFF)
  if(TARGET kickos_chip_${KICKOS_CHIP})
    _kickos_target_cc_sources(kickos_chip_${KICKOS_CHIP} "${PROJECT_SOURCE_DIR}/arch" _srcs)
    foreach(_src IN LISTS _srcs)
      set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                   "${_src}")
      file(READ "${_src}" _txt)
      if(_txt MATCHES "[^A-Za-z0-9_]${symbol}\\([^)]*\\)[ \t\r\n]*{")
        set(_defined ON)
      endif()
    endforeach()
  endif()
  set(${out} ${_defined} PARENT_SCOPE)
endfunction()

# --- Every posture ---------------------------------------------------------------------------
add_subdirectory(hello)
add_subdirectory(hello_c)
add_subdirectory(selftest)
add_subdirectory(stress)
add_subdirectory(sched_exit)
# Its only witness is the kernel block's high-water line, which a panic prints only where the
# board carves the blocks and reports them.
if(KICKOS_KERNEL_STACKS AND KICKOS_KSTACK_REPORT)
  add_subdirectory(stackdepth)
endif()

kickos_add_diagnostic_apps(fault panicgate)

# --- A C library on the target -----------------------------------------------------------
if(KICKOS_HAVE_LIBC)
  add_subdirectory(libc_exit)
  add_subdirectory(sysdefault)
  kickos_add_diagnostic_apps(rootfault rootauth rebootdemo reclaimwit)
endif()
# objbudget fills main's endpoint budget and asks that the refusal be the budget's, so both the
# pool and main's table must have room for that budget: the pool has none to spare where the
# init seats the partition's ports in it first, and main's table is a spawn's width
# (KICKOS_MAX_SPAWN_GRANTS + 1), part of it held from the start.
if(KICKOS_HAVE_LIBC AND NOT KICKOS_AMP_PORTS
   AND KICKOS_TASK_ENDPOINT_BUDGET LESS KICKOS_MAX_SPAWN_GRANTS)
  kickos_add_diagnostic_apps(objbudget)
endif()
# mpu_fault grants a region to one thread of a task, which a backend that translates cannot:
# its unit of memory is the task's space.
if(KICKOS_HAVE_LIBC AND NOT KICKOS_HAVE_ASPACE)
  kickos_add_diagnostic_apps(mpu_fault)
endif()

# --- Memory enforcement --------------------------------------------------------------------
# specfault's verdict is that a read of an address nothing grants is DENIED. Where nothing
# enforces memory the read is the bus's to answer: it returns on esp32-wroom, and faults on
# f302nucleo only because no slave sits there, which is no claim of the kernel's.
if(KICKOS_MEMORY_ENFORCED)
  kickos_add_diagnostic_apps(specfault)
endif()

# --- Fault isolation, translation, and the host sim --------------------------------------
if(KICKOS_FAULT_ISOLATION)
  kickos_add_diagnostic_apps(faultsurvive)
endif()
if(KICKOS_HAVE_ASPACE)
  kickos_add_diagnostic_apps(aspacefault aspaceufault kernelhalf stackguard)
endif()
# Sim only: each drives something that is only automatable on the host, a loopback "device"
# that is the host wire, a published posture, or __builtin_trap.
if(KICKOS_ARCH STREQUAL "sim")
  kickos_add_diagnostic_apps(pubpanic simconabi uartloop drvdeath conrestart)
endif()

# --- Per-ISA ------------------------------------------------------------------------------
# The PSP-bounds and privilege-ring probes move SP with ARM asm and read ARM system registers;
# each main.cc #errors anywhere else.
if(KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m")
  kickos_add_diagnostic_apps(ringpriv pspguard)
endif()
# The nested-trap witness: rv32imac is the one backend that runs syscall dispatch on the
# caller's own stack AND builds its trap frames in software, so it is the only one where an
# interrupt taken mid-dispatch can put a frame at an unbounded depth on a thread's stack. The
# witness it reads is kept OUT of a bench build: its call site is inside what the injected-IRQ
# bench measures. Its deep syscall is a spawn the region rules must refuse, and without an MPU
# backend nothing refuses it.
if(KICKOS_ARCH STREQUAL "rv32imac" AND NOT KICKOS_BENCH AND KICKOS_HAVE_MPU)
  kickos_add_diagnostic_apps(trapnest)
endif()
# Built only where the board compiles TLS: elsewhere the thread_local is a link error naming
# __aeabi_read_tp, which is the intended diagnostic but not a fleet build failure.
if(KICKOS_TLS)
  add_subdirectory(tlsprobe)
endif()
# Built only where an emulator runs it: its only witness is a QEMU gate.
kickos_qemu_machine("${KICKOS_BOARD}" _kos_errno_env _kos_errno_machine)
if(NOT _kos_errno_machine STREQUAL "")
  add_subdirectory(errnoprobe)
endif()
# Double classification and printf through the compiler and newlib, wherever newlib prints a
# double: nano does not, and the sim's C library is the host's.
if(KICKOS_HAVE_LIBC AND NOT KICKOS_ARCH STREQUAL "sim" AND NOT KICKOS_NEWLIB_NANO)
  add_subdirectory(fpclass)
endif()

# Per-thread stdout regression. newlib_stubs.cc (the _write under test) is compiled ONLY for
# the cross toolchains, NOT the sim, so the sim gives zero coverage. The QEMU mps2 images are
# the automatable armv7m gates: they run and exit, and arch_shutdown forwards the exit status
# over semihosting. All threads share one address space there, so the app's globals are a valid
# cross-thread channel.
if(KICKOS_QEMU_MPS2)
  add_subdirectory(initdemo)
endif()

# --- Above one core -----------------------------------------------------------------------
# THE TARGETS THEMSELVES are gated and not just their ctest entries: slaypeer's main.cc pins by
# core mask and reads a printf formatter, and taskleave calls kos_aspace_probe unconditionally,
# which is not declared where KICKOS_HAVE_ASPACE is 0. At one core a victim cannot be RUNNING
# anywhere but where the slayer is, which is the whole hazard.
if(KICKOS_HAVE_LIBC AND KICKOS_BUILD_TESTS AND KICKOS_KERNEL_CORES GREATER 1)
  add_subdirectory(slaypeer)
  # Its judge reads the arm64 fault record, and the arm stands in for an arm64 image gate.
  if(KICKOS_HAVE_ASPACE AND KICKOS_ARCH STREQUAL "armv8a")
    add_subdirectory(unmappeer)
  endif()
  # The counter taskleave reads exists only where a space can be installed on a core at all,
  # and the probe is selftest scaffolding.
  if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST)
    add_subdirectory(taskleave)
  endif()
endif()

# --- The own-image AMP partition ----------------------------------------------------------
# AFTER selftest above: the partition targets consume $<TARGET_FILE:selftest>, and a target has
# to exist before another directory can name it.
if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  add_subdirectory(ampping)
endif()

# --- Full-C++ opt-in smoke test -----------------------------------------------------------
# The toolchain's libstdc++ over newlib, so not the sim, whose runtime and terminate handler are
# the host's; and where the board's heap holds cxxtest's high-water, 6,136 B
# (docs/reference/boards.md). Every link runs --orphan-handling=error, so a linker script that
# homes no EH table refuses the image.
file(STRINGS "${PROJECT_SOURCE_DIR}/boards/${KICKOS_BOARD}/composition.yaml" _kos_heap
     REGEX "^heap: *[0-9]+" LIMIT_COUNT 1)
string(REGEX REPLACE "^heap: *([0-9]+).*$" "\\1" _kos_heap "${_kos_heap}")
if(NOT KICKOS_ARCH STREQUAL "sim" AND _kos_heap GREATER_EQUAL 6136)
  add_subdirectory(cxxtest)
  add_subdirectory(cxxterm)
endif()

# --- Telemetry ----------------------------------------------------------------------------
if(KICKOS_TELEMETRY_ON)
  add_subdirectory(tele_pingpong)
endif()
if(KICKOS_TELEMETRY_ON AND KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(tele_flood)
endif()

# --- FP context-switch torture test -------------------------------------------------------
# Uses vldmia/vstmia {s16-s31}, so an armv7m core with an FPU, which a float ABI other than soft
# states, or any armv8a one.
if((KICKOS_ARCH STREQUAL "armv7m" AND NOT KICKOS_MFLOAT_ABI STREQUAL "soft")
   OR KICKOS_ARCH STREQUAL "armv8a")
  add_subdirectory(fp_switch)
endif()

if(KICKOS_BENCH)
  add_subdirectory(bench)
  # The authority and bound arms on the bench syscall.
  if(KICKOS_HAVE_LIBC)
    add_subdirectory(benchauth)
  endif()
endif()

# --- GPIO from userspace: the boards whose layout gpioblink carries -------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gpioblink/systems/${KICKOS_BOARD}.yaml")
  add_subdirectory(gpioblink)
endif()

# --- Bring-up LED blink -------------------------------------------------------------------
# On a chip whose arch_diag_led_set is the no-op fallback it would blink nothing and read as a
# dead board.
_kos_chip_defines(_kos_led arch_diag_led_set)
if(_kos_led)
  add_subdirectory(blink)
endif()

# Its composition names the board's packaged USB CDC console driver.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/usbcdcwit/systems/${KICKOS_BOARD}.yaml"
   AND NOT KICKOS_AMP_NODE)
  add_subdirectory(usbcdcwit)
endif()

# --- Deliberate HARDWARE harnesses, OFF by default ----------------------------------------
# The knobs are declared unconditionally so they reach the cache and `cmake -LH` on every
# board; only the descent is gated.
option(KICKOS_CLOCK_RETUNE_TEST "clockretune: privileged runtime clock-retune HW harness" OFF)
option(KICKOS_CLOCKSOAK_TEST "clocksoak: clock-hardening wrap/soak HW harness" OFF)
# Only a chip that defines arch_cpu_clock_set, under enforcement; elsewhere the retune is the
# fallback no-op and this harness would prove nothing.
_kos_chip_defines(_kos_clock_set arch_cpu_clock_set)
if(KICKOS_CLOCK_RETUNE_TEST AND _kos_clock_set AND KICKOS_HAVE_MPU)
  add_subdirectory(clockretune)
endif()
# Arch-neutral (clock_now plus sleep_ns only), so it builds for any board.
# Its judge times a capture's arrival stamps, so it is built where a board is captured, not run
# under an emulator.
if(_kos_errno_machine STREQUAL "" AND NOT KICKOS_ARCH STREQUAL "sim" AND NOT KICKOS_AMP_NODE)
  add_subdirectory(wallclock)
endif()
if(KICKOS_CLOCKSOAK_TEST)
  add_subdirectory(clocksoak)
endif()
