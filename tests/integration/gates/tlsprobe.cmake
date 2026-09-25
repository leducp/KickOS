# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The thread-pointer arms riding `tlsprobe`, plus the offline RX replay against the same
# image.

if(NOT TARGET tlsprobe)
  return()
endif()

# One arm per mechanism an emulator can run: mps2 masks SP on armv7m, microbit on armv6m;
# qemu-riscv and armv8a seat the register from the context, and there the image also runs a
# thread on a caller-supplied stack the mask would refuse.
if(KICKOS_CHIP STREQUAL "mps2" OR KICKOS_BOARD STREQUAL "microbit"
   OR KICKOS_BOARD STREQUAL "qemu-riscv" OR KICKOS_ARCH STREQUAL "armv8a")
  kickos_add_qemu_test(TARGET tlsprobe
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_tlsprobe.sh")
endif()

# The RX emutls block is proved offline, rxv3 having no QEMU machine: every input the bump
# allocator uses is a link-time constant, so the allocation is replayed against the linked
# image. Only images in this tree are replayed.
if(KICKOS_ARCH STREQUAL "rxv3")
  add_test(
    NAME    rx_tls_fit
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/rx_tls_fit.py"
            "${CMAKE_NM}" "${CMAKE_OBJDUMP}" "${CMAKE_BINARY_DIR}/user/apps")
  kickos_host_gate(rx_tls_fit TIMEOUT 60)
endif()
