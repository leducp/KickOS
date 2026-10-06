# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The kernel-block depth arm. It boots stackdepth0 (panic and nothing else) and stackdepth1 (the
# grant-carrying spawn, then panic): the block records a maximum, so the difference between the
# two readings is what the spawn arm reached.

if(NOT TARGET stackdepth0)
  return()
endif()
kickos_emulator_judged(stackdepth0 stackdepth1)

# The floor is a DECLARATION, not a measurement: it is the slack the block must keep over the
# paths these two images drive.
set(_sd_floor 256)

# The NAME is spelled out: the derived default names the TARGET image alone, stackdepth0.
if(KICKOS_KERNEL_STACKS AND KICKOS_KSTACK_REPORT)
  kickos_add_qemu_test(NAME ${_tag}_stackdepth TARGET stackdepth0 BOOTS 2
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_stackdepth.sh"
    ARGS "$<TARGET_FILE:stackdepth1>" ${_sd_floor})
endif()
