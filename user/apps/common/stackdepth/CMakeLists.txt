# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Two images off one source. Mode 0 panics and nothing else; mode 1 drives the grant-carrying
# spawn first. The block records a maximum, so the difference between the two readings is what
# the spawn arm reached.
foreach(_mode 0 1)
  kickos_add_application(stackdepth${_mode}
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  if(NOT TARGET stackdepth${_mode})
    return()
  endif()
  target_compile_definitions(stackdepth${_mode} PRIVATE KICKOS_SD_MODE=${_mode})
endforeach()

# The floor is a DECLARATION, not a measurement: it is the slack the block must keep over the
# paths these two images drive.
set(_sd_floor 256)

if(KICKOS_BUILD_TESTS AND KICKOS_KERNEL_STACKS AND KICKOS_KSTACK_REPORT
   AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_stackdepth TARGET stackdepth0 BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_stackdepth.sh"
    ARGS "$<TARGET_FILE:stackdepth1>" ${_sd_floor})
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_KERNEL_STACKS AND KICKOS_KSTACK_REPORT
   AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_stackdepth TARGET stackdepth0
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_stackdepth.sh"
    ARGS "$<TARGET_FILE:stackdepth1>" ${_sd_floor})
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_KERNEL_STACKS AND KICKOS_KSTACK_REPORT
   AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_stackdepth TARGET stackdepth0 BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_stackdepth.sh"
    ARGS "$<TARGET_FILE:stackdepth1>" ${_sd_floor})
endif()
