# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The kernel-block depth arm. It boots stackdepth0 (panic and nothing else) and reads the
# second image, stackdepth1 (the grant-carrying spawn), off disk: the block records a
# maximum, so the difference between the two readings is what the spawn arm reached.

if(NOT TARGET stackdepth0)
  return()
endif()

# The floor is a DECLARATION, not a measurement: it is the slack the block must keep over the
# paths these two images drive.
set(_sd_floor 256)

# ONE call for the three boards that carried an arm before: the script, the second image and
# the floor are identical on all three, and the NAME is spelled out because the derived
# default would name the booted target, stackdepth0. The board predicate stays: the other
# emulatable boards registered no arm here and must keep registering none.
if(KICKOS_KERNEL_STACKS AND KICKOS_KSTACK_REPORT
   AND (KICKOS_BOARD STREQUAL "qemu-riscv" OR KICKOS_CHIP STREQUAL "mps2"
        OR KICKOS_BOARD STREQUAL "microbit"))
  kickos_add_qemu_test(NAME ${_tag}_stackdepth TARGET stackdepth0
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_stackdepth.sh"
    ARGS "$<TARGET_FILE:stackdepth1>" ${_sd_floor})
endif()
