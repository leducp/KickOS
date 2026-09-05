# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The runnable QEMU MPS2 boards: one chip backend (mps2) behind four FPGA images, an386/M4,
# an505/M33 (PMSAv8), an500/M7 (16 MPU regions) and an385/M3 (soft-float). qemu-m33 has its own
# memory map (boards/qemu-m33/mps2.ld); the other three take the chip's. Inherited by every
# add_subdirectory below.
set(KICKOS_QEMU_MPS2_BOARDS qemu qemu-m33 qemu-m7 qemu-m3)
# The board name as a CTest-name prefix, so each image reports under its own name.
string(REPLACE "-" "_" KICKOS_QEMU_MPS2_TAG "${KICKOS_BOARD}")

# The runnable QEMU arm64 boards. Every gate keyed on this list reads an ARCH-level fault
# marker or status, so the two take the same scripts and differ only in the name each reports
# under. imx8mp-evk brings up one core, so the SMP gates stay keyed on the core count.
set(KICKOS_QEMU_ARM64_BOARDS qemu-arm64 imx8mp-evk)

add_subdirectory(hello)
add_subdirectory(hello_c)
add_subdirectory(mpu_fault)
add_subdirectory(rootfault)
add_subdirectory(rootauth)
add_subdirectory(ringpriv)
add_subdirectory(panicgate)
add_subdirectory(pubpanic)
add_subdirectory(drvdeath)
add_subdirectory(reclaimwit)
add_subdirectory(simconabi)
add_subdirectory(uartloop)
add_subdirectory(sched_exit)
add_subdirectory(libc_exit)

add_subdirectory(selftest)
# AFTER selftest: the AMP partition targets consume $<TARGET_FILE:selftest>, and a target has to
# exist before another directory can name it.
add_subdirectory(ampping)
add_subdirectory(stress)

add_subdirectory(gpioblink)

add_subdirectory(fault)
add_subdirectory(aspacefault)
add_subdirectory(aspaceufault)
add_subdirectory(stackguard)
add_subdirectory(kernelhalf)
add_subdirectory(faultsurvive)
add_subdirectory(pspguard)
add_subdirectory(trapnest)
add_subdirectory(specfault)
add_subdirectory(tlsprobe)

add_subdirectory(errnoprobe)
add_subdirectory(rebootdemo)

add_subdirectory(initdemo)

add_subdirectory(clockretune)

add_subdirectory(clocksoak)

# Full-C++ opt-in smoke test. A board joins this list only once its RAM covers the runtime heap
# and its linker script homes the toolchain EH tables (EHABI .ARM.extab / DWARF
# .eh_frame+.gcc_except_table / RX SjLj .gcc_except_table) plus the runtime writable state
# inside the app grants.
if(KICKOS_BOARD STREQUAL "frdmk64f"
   OR KICKOS_BOARD STREQUAL "xmc4800-relax"
   OR KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS
   OR ((KICKOS_BOARD STREQUAL "picopi"
        OR KICKOS_BOARD STREQUAL "rx72m"
        OR KICKOS_BOARD STREQUAL "esp32c6-wroom"
        OR KICKOS_BOARD STREQUAL "qemu-riscv")
       AND KICKOS_HAVE_MPU)
   OR (KICKOS_BOARD STREQUAL "qemu-riscv" AND NOT KICKOS_HAVE_MPU)
   OR KICKOS_BOARD STREQUAL "qemu-riscv64")
  add_subdirectory(cxxtest)
endif()

if(KICKOS_TELEMETRY_ON AND (KICKOS_ARCH STREQUAL "sim" OR KICKOS_BOARD STREQUAL "qemu"))
  add_subdirectory(tele_pingpong)
endif()
if(KICKOS_TELEMETRY_ON AND KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(tele_flood)
endif()

# FP context-switch torture test. Uses vldmia/vstmia {s16-s31}, so armv7m+FPU only: the QEMU
# images are enumerated here rather than taken from KICKOS_QEMU_MPS2_BOARDS, qemu-m3 (an385,
# Cortex-M3) being soft-float.
if(KICKOS_BOARD STREQUAL "frdmk64f"
   OR KICKOS_BOARD STREQUAL "xmc4800-relax"
   OR KICKOS_BOARD STREQUAL "qemu"
   OR KICKOS_BOARD STREQUAL "qemu-m33"
   OR KICKOS_BOARD STREQUAL "qemu-m7"
   OR KICKOS_BOARD IN_LIST KICKOS_QEMU_ARM64_BOARDS)
  add_subdirectory(fp_switch)
endif()

if(KICKOS_BENCH)
  add_subdirectory(bench)
endif()

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
