# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# THIS FILE SELECTS. Every app below is entered only on a posture where it applies, so no app
# file asks whether it applies: an app declares its target and stops. What an app file may
# still hold is a precondition that is the APP's own and carries operator instructions with it
# (a service list that must publish a console, a knob that shapes the image), never a
# board-level fact that is readable here.
#
# Two board groupings recur, and both are arch/chip facts rather than lists to maintain:
#   the four QEMU MPS2 FPGA images (an386/M4F, an505/M33, an500/M7, an385/M3) are exactly the
#     boards on the `mps2` chip backend, KICKOS_CHIP STREQUAL "mps2". qemu-m33 has its own
#     memory map (boards/qemu-m33/mps2.ld); the other three take the chip's.
#   the two emulatable arm64 boards (qemu-arm64, imx8mp-evk) are exactly the armv8a fleet,
#     KICKOS_ARCH STREQUAL "armv8a".

# --- Every posture ---------------------------------------------------------------------------
add_subdirectory(hello)
add_subdirectory(hello_c)
add_subdirectory(selftest)
add_subdirectory(stress)
add_subdirectory(sched_exit)
add_subdirectory(stackdepth)

kickos_add_diagnostic_apps(fault panicgate specfault)

# --- A C library on the target -----------------------------------------------------------
if(KICKOS_HAVE_LIBC)
  add_subdirectory(gpioblink)
  add_subdirectory(libc_exit)
  kickos_add_diagnostic_apps(mpu_fault rootfault rootgone rootauth rebootdemo reclaimwit
                             objbudget)
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
  kickos_add_diagnostic_apps(pubpanic simconabi uartloop drvdeath)
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
# bench measures.
if(KICKOS_ARCH STREQUAL "rv32imac" AND NOT KICKOS_BENCH)
  kickos_add_diagnostic_apps(trapnest)
endif()
# Built only where the board compiles TLS: elsewhere the thread_local is a link error naming
# __aeabi_read_tp, which is the intended diagnostic but not a fleet build failure.
if(KICKOS_TLS)
  add_subdirectory(tlsprobe)
endif()
# The reentrancy seat is a target-libc notion; the sim bridges to the host's and x86_64 links
# no C library at all.
if(NOT KICKOS_ARCH STREQUAL "sim" AND NOT KICKOS_ARCH STREQUAL "x86_64")
  add_subdirectory(errnoprobe)
endif()

# Per-thread stdout regression. newlib_stubs.cc (the _write under test) is compiled ONLY for
# the cross toolchains, NOT the sim, so the sim gives zero coverage. The QEMU mps2 images are
# the automatable armv7m gates: they run and exit, and arch_shutdown forwards the exit status
# over semihosting. All threads share one address space there, so the app's globals are a valid
# cross-thread channel.
if(KICKOS_CHIP STREQUAL "mps2")
  add_subdirectory(initdemo)
endif()

# --- Above one core -----------------------------------------------------------------------
# THE TARGETS THEMSELVES are gated and not just their ctest entries: slaypeer's main.cc pins by
# core mask and reads a printf formatter, and taskleave calls kos_aspace_probe unconditionally,
# which is not declared where KICKOS_HAVE_ASPACE is 0. At one core a victim cannot be RUNNING
# anywhere but where the slayer is, which is the whole hazard.
if(KICKOS_HAVE_LIBC AND KICKOS_BUILD_TESTS AND KICKOS_KERNEL_CORES GREATER 1)
  add_subdirectory(slaypeer)
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
# A board joins this list only once its RAM covers the runtime heap and its linker script homes
# the toolchain EH tables (EHABI .ARM.extab / DWARF .eh_frame+.gcc_except_table / RX SjLj
# .gcc_except_table) plus the runtime writable state inside the app grants.
if(KICKOS_BOARD STREQUAL "frdmk64f"
   OR KICKOS_BOARD STREQUAL "xmc4800-relax"
   OR KICKOS_CHIP STREQUAL "mps2"
   OR ((KICKOS_BOARD STREQUAL "picopi"
        OR KICKOS_BOARD STREQUAL "rx72m"
        OR KICKOS_BOARD STREQUAL "esp32c6-wroom"
        OR KICKOS_BOARD STREQUAL "qemu-riscv")
       AND KICKOS_HAVE_MPU)
   OR (KICKOS_BOARD STREQUAL "qemu-riscv" AND NOT KICKOS_HAVE_MPU)
   OR KICKOS_BOARD STREQUAL "qemu-riscv64")
  add_subdirectory(cxxtest)
endif()

# --- Telemetry ----------------------------------------------------------------------------
if(KICKOS_TELEMETRY_ON AND (KICKOS_ARCH STREQUAL "sim" OR KICKOS_BOARD STREQUAL "qemu"))
  add_subdirectory(tele_pingpong)
endif()
if(KICKOS_TELEMETRY_ON AND KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(tele_flood)
endif()

# --- FP context-switch torture test -------------------------------------------------------
# Uses vldmia/vstmia {s16-s31}, so armv7m+FPU only: the QEMU images are enumerated here rather
# than taken from the mps2 chip, qemu-m3 (an385, Cortex-M3) being soft-float.
if(KICKOS_BOARD STREQUAL "frdmk64f"
   OR KICKOS_BOARD STREQUAL "xmc4800-relax"
   OR KICKOS_BOARD STREQUAL "qemu"
   OR KICKOS_BOARD STREQUAL "qemu-m33"
   OR KICKOS_BOARD STREQUAL "qemu-m7"
   OR KICKOS_ARCH STREQUAL "armv8a")
  add_subdirectory(fp_switch)
endif()

if(KICKOS_BENCH)
  add_subdirectory(bench)
endif()

# --- Bring-up LED blink -------------------------------------------------------------------
# An ALLOWLIST: on a chip whose arch_diag_led_set is the no-op fallback it would blink nothing
# and read as a dead board.
if(KICKOS_BOARD STREQUAL "picopi"
   OR KICKOS_BOARD STREQUAL "frdmk64f"
   OR KICKOS_BOARD STREQUAL "f411disco"
   OR KICKOS_BOARD STREQUAL "blackpill"
   OR KICKOS_BOARD STREQUAL "bluepill-c8"
   OR KICKOS_BOARD STREQUAL "f302nucleo"
   OR KICKOS_BOARD STREQUAL "due"
   OR KICKOS_BOARD STREQUAL "xmc4800-relax"
   OR KICKOS_BOARD STREQUAL "esp32-wroom"
   OR KICKOS_BOARD STREQUAL "teensy41")
  add_subdirectory(blink)
endif()

# Without a CDC driver cap 0 is the kernel console and the app measures nothing.
if(KICKOS_SERVICE_LIST MATCHES "_usbcdc$")
  add_subdirectory(usbcdcwit)
endif()

# --- Deliberate HARDWARE harnesses, OFF by default ----------------------------------------
# The knobs are declared unconditionally so they reach the cache and `cmake -LH` on every
# board; only the descent is gated.
option(KICKOS_CLOCK_RETUNE_TEST "clockretune: privileged runtime clock-retune HW harness" OFF)
option(KICKOS_CLOCKSOAK_TEST "clocksoak: clock-hardening wrap/soak HW harness" OFF)
# Only the two backends that strong-override arch_cpu_clock_set (XMC4800, K64F) under
# enforcement; elsewhere the retune is a fallback no-op and this harness would prove nothing.
if(KICKOS_CLOCK_RETUNE_TEST
   AND (KICKOS_BOARD STREQUAL "xmc4800-relax" OR KICKOS_BOARD STREQUAL "frdmk64f")
   AND KICKOS_HAVE_MPU)
  add_subdirectory(clockretune)
endif()
# Arch-neutral (clock_now plus sleep_ns only), so it builds for any board.
if(KICKOS_CLOCKSOAK_TEST)
  add_subdirectory(clocksoak)
endif()
