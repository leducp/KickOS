# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_subdirectory(hello)
add_subdirectory(hello_c)
add_subdirectory(selftest)
add_subdirectory(mpu_fault)
add_subdirectory(sched_exit)
add_subdirectory(stress)

# Diagnostic apps -- self-gated on KICKOS_ENABLE_SELFTEST via kickos_add_diagnostic_app
# (see each CMakeLists), so no per-board list here. `fault` deliberately executes an
# illegal instruction; it is never built into a production image.
add_subdirectory(fault)

# KickCAT EtherCAT slave over the software EmulatedESC (Stage A of
# docs/design-kickcat-k64f.md). Sim-only; consumes the sibling KickCAT checkout
# (used, not vendored) and self-skips if it is absent.
if(KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(kickcat_slave)
endif()

# Full-C++ opt-in smoke test (Stage B of docs/design-kickcat-k64f.md): exercises
# exceptions + STL + RTTI over the toolchain libstdc++/newlib. Gated to the K64F
# silicon target and the runnable QEMU armv7m/RISC-V verification boards -- boards
# with enough RAM for the 64 KB libc heap arena and where it is actually run.
# NOT under MPU enforcement (KICKOS_HAVE_MPU): libstdc++'s EH unwind reads the DWARF
# frame registry + .eh_frame + its malloc'd FDE node from kernel-owned memory -- a
# cross-domain read an unprivileged thread would fault on under PMP/PMSA -- and the
# full-C++ image also overflows the per-app NAPOT code region. Full-C++ under
# enforcement (granting the EH tables + heap to the domain, or a privileged slave
# thread) is a later stage.
if((KICKOS_BOARD STREQUAL "frdmk64f"
    OR KICKOS_BOARD STREQUAL "qemu"
    OR KICKOS_BOARD STREQUAL "qemu-riscv")
   AND NOT KICKOS_HAVE_MPU)
  add_subdirectory(cxxtest)
endif()

# K64F GPIO/timer first driver over the MMIO grant -- chip-specific (PIT + SYSMPU),
# so only for the frdmk64f board; also self-gates on KICKOS_ENABLE_SELFTEST.
if(KICKOS_BOARD STREQUAL "frdmk64f")
  add_subdirectory(k64drv)
endif()

# K64F/DSPI0 unprivileged SPI driver -- the real KickCAT-target SPI substrate
# (SPI0/PORTD/AIPS bring-up + SOUT->SIN loopback), so only for the frdmk64f board
# and enforcement builds; also self-gates on KICKOS_ENABLE_SELFTEST.
if(KICKOS_BOARD STREQUAL "frdmk64f" AND KICKOS_HAVE_MPU)
  add_subdirectory(k64dspi)
endif()

# STM32F411 SPI1 loopback driver -- the canonical per-thread peripheral-MMIO
# isolation reference on PMSA (SPI1/GPIOA/RCC bring-up), so only for the f411disco
# board; also self-gates on KICKOS_ENABLE_SELFTEST.
if(KICKOS_BOARD STREQUAL "f411disco" AND KICKOS_HAVE_MPU)
  add_subdirectory(f411spi)
endif()

# XMC4800 USIC0-CH1 SSC (SPI) internal-loopback driver -- the PMSA-proven M5
# board-support / userspace-library SPI reference (USIC/SCU bring-up), so only for
# the xmc4800-relax board and enforcement builds; also self-gates on
# KICKOS_ENABLE_SELFTEST.
if(KICKOS_BOARD STREQUAL "xmc4800-relax" AND KICKOS_HAVE_MPU)
  add_subdirectory(xmcspi)
endif()

# ESP32-C6 GPIO10 blink -- the canonical per-thread peripheral-MMIO isolation
# reference on RISC-V PMP (GPIO/IO_MUX/APM bring-up + the one-time APM open), so only
# for the esp32c6-wroom board and enforcement builds; also self-gates on
# KICKOS_ENABLE_SELFTEST.
if(KICKOS_BOARD STREQUAL "esp32c6-wroom" AND KICKOS_HAVE_MPU)
  add_subdirectory(c6blink)
endif()

# RX72M GPIO blink -- the per-thread peripheral-MMIO isolation reference on the RX MPU
# (PORT8 bring-up + a granted 16 B PODR window), so only for the rx72m board and
# enforcement builds; also self-gates on KICKOS_ENABLE_SELFTEST.
if(KICKOS_BOARD STREQUAL "rx72m" AND KICKOS_HAVE_MPU)
  add_subdirectory(rxdrv)
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
# Cortex-M4F for the in-env gate). Uses vldmia/vstmia {s16-s31}, so armv7m+FPU only.
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
   OR KICKOS_BOARD STREQUAL "bluepill"
   OR KICKOS_BOARD STREQUAL "bluepill-c8"
   OR KICKOS_BOARD STREQUAL "f302nucleo"
   OR KICKOS_BOARD STREQUAL "due"
   OR KICKOS_BOARD STREQUAL "xmc4800-relax"
   OR KICKOS_BOARD STREQUAL "esp32-wroom")
  add_subdirectory(blink)
endif()
