# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The runnable QEMU MPS2 boards. One chip backend (mps2) can stand behind several FPGA
# images, which all share a memory map, a console and a clock -- so the gates below are
# per-IMAGE facts, not per-chip ones, and a gate that holds on one image holds on any
# other the arch backend claims to support. Named once here rather than repeated as an
# N-way OR at each of the eight call sites that used to read
# `KICKOS_BOARD STREQUAL "qemu"`, so adding an image is a one-line change.
# Inherited by every add_subdirectory below.
set(KICKOS_QEMU_MPS2_BOARDS qemu)
# The board name as a CTest-name prefix, so each image reports under its own name and
# the an386 board keeps the names it has always had (qemu_hello, qemu_selftest, ...).
string(REPLACE "-" "_" KICKOS_QEMU_MPS2_TAG "${KICKOS_BOARD}")

add_subdirectory(hello)
add_subdirectory(hello_c)
add_subdirectory(mpu_fault)
add_subdirectory(sched_exit)

add_subdirectory(selftest)
add_subdirectory(stress)

# GPIO pin-allocator demo (M4.3): board-agnostic (the pin is two compile defs), so
# built for every board -- on a chip with no GPIO backend claim returns -KOS_ENOSYS
# and the app parks.
add_subdirectory(gpioblink)

# Diagnostic apps -- self-gated on KICKOS_ENABLE_SELFTEST via kickos_add_diagnostic_app
# (see each CMakeLists), so no per-board list here. `fault` deliberately executes an
# illegal instruction; it is never built into a production image.
add_subdirectory(fault)
add_subdirectory(specfault)

# Per-thread stdout classification regression (M4.3): a root pre-publish printf must
# not poison a post-publish worker's route to the console endpoint. Software-only
# (no MMIO) counting console driver; the verdict rides the exit status. Self-gates on
# the qemu (mps2) board in its own CMakeLists (the only place newlib_stubs is compiled
# AND the run is automatable; sim never compiles newlib_stubs, XMC has no QEMU model).
add_subdirectory(initdemo)

# Privileged runtime clock-retune HW harness -- self-gates on KICKOS_CLOCK_RETUNE_TEST
# (OFF by default) + xmc4800-relax/frdmk64f + enforcement in its own CMakeLists.
add_subdirectory(clockretune)

# Clock-hardening wrap/soak HW harness -- self-gates on KICKOS_CLOCKSOAK_TEST (OFF by
# default) in its own CMakeLists; arch-neutral, builds for any board.
add_subdirectory(clocksoak)

# Full-C++ opt-in smoke test (Stage B of docs/design-kickcat-k64f.md): exercises
# exceptions + STL + RTTI over the toolchain libstdc++/newlib. Gated to the K64F
# silicon target, the runnable QEMU armv7m/RISC-V verification boards, and the 64K-flash
# boards that fit after the M4.5 vterminate reclaim (a strong __verbose_terminate_handler
# stops the linker pulling libstdc++'s demangle + float dtoa, ~63K .text). Each board's
# RAM must also cover the runtime heap: the freestanding chips carve a fixed .userheap
# (the MPU chips route the runtime writable state into the granted .appdata window).
# frdmk64f builds under MPU enforcement too -- the KickOS full-C++-under-isolation gate.
# mk64f.ld routes libkickos_user.a's heap arena + the libstdc++ writable state into the
# granted .appdata/.appbss window (archive:member colon-inclusion of the closed kernel
# set), and the EHABI .exidx tables are read-only in flash, reached via the code grant --
# so an unprivileged thread runs the full runtime (the freedom-k64f slave reached
# OPERATIONAL under SYSMPU). The operator runs cxxtest on K64F silicon.
# picopi (rp2040/EHABI), rx72m (RXv3/SjLj) and esp32c6-wroom (rv32imac/DWARF) build
# cxxtest ONLY under enforcement -- each chip .ld homes the toolchain EH tables (EHABI
# .ARM.extab / DWARF .eh_frame+.gcc_except_table / RX SjLj .gcc_except_table) into the
# app code+data grants and routes the runtime writable state into the .appdata window.
# The QEMU mps2 images stay non-enforcement (no enforcement block). qemu-riscv builds
# BOTH: non-enforcement (freestanding path) and, when KICKOS_HAVE_MPU is set, the
# full-C++ throw under PMP -- the deterministic runtime gate for the gp-in-appdata
# layout (docs/design-riscv-gp-split.md), no silicon needed.
if(KICKOS_BOARD STREQUAL "frdmk64f"
   OR KICKOS_BOARD STREQUAL "xmc4800-relax"
   OR ((KICKOS_BOARD STREQUAL "picopi"
        OR KICKOS_BOARD STREQUAL "rx72m"
        OR KICKOS_BOARD STREQUAL "esp32c6-wroom"
        OR KICKOS_BOARD STREQUAL "qemu-riscv")
       AND KICKOS_HAVE_MPU)
   OR ((KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS
        OR KICKOS_BOARD STREQUAL "qemu-riscv")
       AND NOT KICKOS_HAVE_MPU)
   # bluepill-c8 (stm32f103, 64K/20K, silicon non-MPU): cxxtest is 57K .text after the
   # vterminate reclaim (fits 64K flash) and its 8K .userheap clears the 5.9K measured
   # cxxtest heap peak. Operator-run on silicon (no QEMU model).
   # f302nucleo (stm32f302, 64K/16K) is deliberately NOT here: cxxtest fits its flash too,
   # but the 4K .userheap the 16K SRAM allows is below the 5.9K peak, so malloc would fail
   # at runtime. Not inflating the heap to force it (no-deferral).
   OR KICKOS_BOARD STREQUAL "bluepill-c8")
  add_subdirectory(cxxtest)
endif()

# Telemetry gate apps. tele_pingpong (bounded, self-terminating) also drives the
# QEMU structural gate (it exits, so mps2 flushes the ring via semihosting), so it
# builds for the sim AND the qemu board when telemetry is on. tele_flood's
# ring-wrap gate is sim-only.
if(KICKOS_TELEMETRY_ON AND (KICKOS_ARCH STREQUAL "sim" OR KICKOS_BOARD STREQUAL "qemu"))
  add_subdirectory(tele_pingpong)
endif()
if(KICKOS_TELEMETRY_ON AND KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(tele_flood)
endif()

# FP context-switch torture test: builds for the M4F dev boards (and the QEMU
# Cortex-M4F for the in-env gate). Uses vldmia/vstmia {s16-s31}, so armv7m+FPU only --
# which is why the QEMU images are enumerated here instead of taken from
# KICKOS_QEMU_MPS2_BOARDS: an FPU is a core capability, not a per-image gate.
if(KICKOS_BOARD STREQUAL "frdmk64f"
   OR KICKOS_BOARD STREQUAL "xmc4800-relax"
   OR KICKOS_BOARD STREQUAL "qemu")
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
   OR KICKOS_BOARD STREQUAL "esp32-wroom")
  add_subdirectory(blink)
endif()
