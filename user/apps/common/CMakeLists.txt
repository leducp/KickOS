# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The runnable QEMU MPS2 boards, in the order below: ONE chip backend (mps2) behind four
# FPGA images, an386/M4, an505/M33 (PMSAv8), an500/M7 (16 MPU regions) and an385/M3
# (soft-float). They share a console and a clock, so a gate that holds on one image holds on
# all of them. qemu-m33 has its own memory map (boards/qemu-m33/mps2.ld); the other three
# take the chip's. Inherited by every add_subdirectory below.
set(KICKOS_QEMU_MPS2_BOARDS qemu qemu-m33 qemu-m7 qemu-m3)
# The board name as a CTest-name prefix, so each image reports under its own name
# (qemu_hello, qemu_m33_selftest, ...).
string(REPLACE "-" "_" KICKOS_QEMU_MPS2_TAG "${KICKOS_BOARD}")

add_subdirectory(hello)
add_subdirectory(hello_c)
add_subdirectory(mpu_fault)
# Root confinement (mpu_fault covers a spawned child).
add_subdirectory(rootfault)
# Root's AUTHORITY narrow (rootfault covers its memory confinement).
add_subdirectory(rootauth)
# The CPU privilege RING. mpu_fault and rootfault prove MEMORY confinement; this is the
# other axis.
add_subdirectory(ringpriv)
# kos_panic on the wire: one image per case, because a run observes one panic.
add_subdirectory(panicgate)
# The same wire, POST-HANDOVER: kos_panic and a CPU fault with a userspace driver owning
# the console. Its gate builds its own published tree.
add_subdirectory(pubpanic)
# The same wire again, when the DRIVER dies rather than the system: the console must come
# back to the kernel instead of going permanently mute.
add_subdirectory(drvdeath)
# The same property on SILICON, plus the terminate drain: the app publishes the console to a
# driver it spawns itself, so it needs no service list.
add_subdirectory(reclaimwit)
# The FRAMED arm of that same endpoint: a console driver must answer a kos_call rather
# than write the request frame out as text and leave the caller parked. Its gate builds its
# own published tree.
add_subdirectory(simconabi)
# The buffered UART end to end against the real two-thread driver, over the sim's loopback
# device. Its gate builds its own tree.
add_subdirectory(uartloop)
add_subdirectory(sched_exit)
# The C library's own exit() on every port: it must link and reach KOS_SYS_EXIT.
add_subdirectory(libc_exit)

add_subdirectory(selftest)
add_subdirectory(stress)

# GPIO pin-allocator demo (M4.3): board-agnostic (the pin is two compile defs), so built for
# every board. A chip whose register layout is not known there compiles to a park-only stub.
add_subdirectory(gpioblink)

# Diagnostic apps, self-gated on KICKOS_ENABLE_SELFTEST via kickos_add_diagnostic_app
# (see each CMakeLists), so no per-board list here. `fault` deliberately executes an
# illegal instruction; it is never built into a production image.
add_subdirectory(fault)
# The map editor's fourth transition: a page unmapped under the RUNNING translation must
# fault. Self-gated on a translating backend, so only qemu-arm64 builds it today.
add_subdirectory(aspacefault)
# The guard page below a mapped thread stack: an overrun must fault instead of reaching a
# neighbour. Self-gated the same way, a region backend having no unmapped page to find.
add_subdirectory(stackguard)
# The kernel's half from EL0: a word of kernel writable state must refuse an unprivileged
# read. Self-gated the same way, a region backend refusing it by descriptor instead.
add_subdirectory(kernelhalf)
add_subdirectory(faultsurvive)
# The PSP-bounds security regression: a thread aims its PSP just above its own stack base,
# where the hardware frame fits and the switcher's software block does not.
add_subdirectory(pspguard)
# The nested-trap security regression: an interrupt taken while the kernel is running a
# thread's syscall dispatch must put its frame on a trusted stack.
add_subdirectory(trapnest)
add_subdirectory(specfault)
# The thread_local witness: per-thread storage at a per-thread ADDRESS, with the .tdata
# template arriving in a freshly spawned thread. Its own CMakeLists returns early where the
# board compiles no TLS.
add_subdirectory(tlsprobe)

# The per-thread errno witness: libc's whole reentrant state following the running thread.
# Its own CMakeLists returns early on the sim, whose libc is the host's.
add_subdirectory(errnoprobe)
# kos_reboot plumbing witness + the unattended-reflash affordance. Board-agnostic;
# its ctest arms are the declining-fallback targets only (see its CMakeLists).
add_subdirectory(rebootdemo)

# Per-thread stdout classification regression (M4.3): a root pre-publish printf must
# not poison a post-publish worker's route to the console endpoint. Software-only
# (no MMIO) counting console driver; the verdict rides the exit status. Self-gates on
# the qemu (mps2) boards in its own CMakeLists: they are the automatable armv7m gates,
# and the sim never compiles newlib_stubs.cc, which holds the _write under test.
add_subdirectory(initdemo)

# Privileged runtime clock-retune HW harness, self-gates on KICKOS_CLOCK_RETUNE_TEST
# (OFF by default) + xmc4800-relax/frdmk64f + enforcement in its own CMakeLists.
add_subdirectory(clockretune)

# Clock-hardening wrap/soak HW harness, self-gates on KICKOS_CLOCKSOAK_TEST (OFF by
# default) in its own CMakeLists; arch-neutral, builds for any board.
add_subdirectory(clocksoak)

# Full-C++ opt-in smoke test (Stage B of docs/design-kickcat-k64f.md): exercises
# exceptions + STL + RTTI over the toolchain libstdc++/newlib. It fits a 64K-flash board
# after the M4.5 vterminate reclaim (a strong __verbose_terminate_handler stops the linker
# pulling libstdc++'s demangle + float dtoa, ~63K .text). Each board's
# RAM must also cover the runtime heap: the freestanding chips carve a fixed .userheap
# (the MPU chips route the runtime writable state into the granted .appdata window).
# frdmk64f builds under MPU enforcement too, the KickOS full-C++-under-isolation gate.
# mk64f.ld routes libkickos_user.a's heap arena + the libstdc++ writable state into the
# granted .appdata/.appbss window (archive:member colon-inclusion of the closed kernel
# set), and the EHABI .exidx tables are read-only in flash, reached via the code grant,
# so an unprivileged thread runs the full runtime (the freedom-k64f slave reached
# OPERATIONAL under SYSMPU). The operator runs cxxtest on K64F silicon.
# picopi (rp2040/EHABI), rx72m (RXv3/SjLj) and esp32c6-wroom (rv32imac/DWARF) build
# cxxtest ONLY under enforcement: each chip .ld homes the toolchain EH tables (EHABI
# .ARM.extab / DWARF .eh_frame+.gcc_except_table / RX SjLj .gcc_except_table) into the
# app code+data grants and routes the runtime writable state into the .appdata window.
# The QEMU mps2 images build it in BOTH postures: the non-enforcement build is the
# plain libstdc++ smoke, and the same gate under KICKOS_HAVE_MPU runs the whole
# runtime (throw/unwind, STL, RTTI) as an unprivileged thread; on qemu-m33 that is
# the only runtime coverage of full C++ under PMSAv8 in the fleet (the other armv8-M
# enforcing ports are all silicon). qemu-riscv likewise builds both: the full-C++
# throw under PMP is the deterministic gate for the gp-in-appdata layout
# (docs/design-riscv-gp-split.md), no silicon needed.
if(KICKOS_BOARD STREQUAL "frdmk64f"
   OR KICKOS_BOARD STREQUAL "xmc4800-relax"
   OR KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS
   OR ((KICKOS_BOARD STREQUAL "picopi"
        OR KICKOS_BOARD STREQUAL "rx72m"
        OR KICKOS_BOARD STREQUAL "esp32c6-wroom"
        OR KICKOS_BOARD STREQUAL "qemu-riscv")
       AND KICKOS_HAVE_MPU)
   OR (KICKOS_BOARD STREQUAL "qemu-riscv" AND NOT KICKOS_HAVE_MPU))
  add_subdirectory(cxxtest)
endif()

# Telemetry gate apps. tele_pingpong is bounded and self-terminating, so it exits and mps2
# flushes the ring via semihosting, which is what drives the QEMU structural gate.
if(KICKOS_TELEMETRY_ON AND (KICKOS_ARCH STREQUAL "sim" OR KICKOS_BOARD STREQUAL "qemu"))
  add_subdirectory(tele_pingpong)
endif()
if(KICKOS_TELEMETRY_ON AND KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(tele_flood)
endif()

# FP context-switch torture test: builds for the M4F dev boards (and the QEMU
# Cortex-M4F for the in-env gate). Uses vldmia/vstmia {s16-s31}, so armv7m+FPU only:
# the QEMU images are enumerated instead of taken from KICKOS_QEMU_MPS2_BOARDS
# because qemu-m3 (an385, Cortex-M3) is soft-float.
if(KICKOS_BOARD STREQUAL "frdmk64f"
   OR KICKOS_BOARD STREQUAL "xmc4800-relax"
   OR KICKOS_BOARD STREQUAL "qemu"
   OR KICKOS_BOARD STREQUAL "qemu-m33"
   OR KICKOS_BOARD STREQUAL "qemu-m7"
   # A64 stacks no FP state on exception entry, so this board owes the whole vector bank
   # rather than the callee-saved half the M-profile boards owe.
   OR KICKOS_BOARD STREQUAL "qemu-arm64")
  add_subdirectory(fp_switch)
endif()

# Context-switch microbenchmark. Built for EVERY arch in a KICKOS_BENCH build: the
# throughput metric (ctx-switches/s via the monotonic clock) is universal. Per-switch
# cycle cost + IRQ latency are reported only where switch.S brackets the swap with a
# cycle counter (armv7m DWT, rxv3 CMTW1, rv32imac rdcycle/MTIME-seam, xtensa CCOUNT);
# on M0 / sim / frozen-QEMU-DWT the bench reports throughput only (scnt==0 at runtime).
if(KICKOS_BENCH)
  add_subdirectory(bench)
endif()

# Board-specific bring-up demo: blinks an onboard LED (boards with a known LED).
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
add_subdirectory(usbcdcwit)
