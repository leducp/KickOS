# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The thread-pointer arms riding `tlsprobe`, plus the offline RX replay against the same
# image.

if(NOT TARGET tlsprobe)
  return()
endif()

# ONE call for the four keys that carried an arm: the script is the same on all of them and
# each reports under the derived <board tag>_tlsprobe name. The keys are kept rather than
# folded away because they are separate claims, and because the remaining emulatable boards
# registered no arm here:
#   mps2 is the armv7m witness;
#   microbit is the armv6m arm, which that witness does not speak for;
#   qemu-riscv is the RISC-V arm;
#   armv8a is the only arch in the fleet that SEATS the thread pointer instead of masking SP
#   down to a stride, so its arm is what says the seating is real rather than the mask by
#   another name.
if(KICKOS_CHIP STREQUAL "mps2" OR KICKOS_BOARD STREQUAL "microbit"
   OR KICKOS_BOARD STREQUAL "qemu-riscv" OR KICKOS_ARCH STREQUAL "armv8a")
  kickos_add_qemu_test(TARGET tlsprobe
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_tlsprobe.sh")
endif()

# The RX emutls block is proved offline: rxv3 has no QEMU machine, so exhausting it is a
# first-touch PANIC nobody can run. Every input the bump allocator uses is a link-time
# constant, so the allocation is replayed against the linked image instead. GAP: this is the
# only RX image in the tree that declares a thread_local, so a consumer app that declares more
# is not covered.
if(KICKOS_ARCH STREQUAL "rxv3")
  add_test(
    NAME    rx_tls_fit
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/rx_tls_fit.py"
            "${CMAKE_NM}" "${CMAKE_OBJDUMP}" "${CMAKE_BINARY_DIR}/user/apps")
  set_tests_properties(rx_tls_fit PROPERTIES TIMEOUT 60 LABELS host)
endif()
