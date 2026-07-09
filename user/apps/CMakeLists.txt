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

# Context-switch microbenchmark: only in KICKOS_BENCH builds (needs the PendSV
# CYCCNT bracket + the kernel accumulator). armv7m + DWT boards (and QEMU for the
# plumbing; its DWT may be frozen, so the real number comes from silicon).
if(KICKOS_BENCH AND (KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "rv32imac" OR KICKOS_ARCH STREQUAL "rxv3"))
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
