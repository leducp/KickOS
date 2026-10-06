# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The QEMU machine a board runs on against its chip file (kickos_qemu_machine): the microbit's
# SRAM against the RAM link region.
if(KICKOS_BOARD STREQUAL "microbit")
  add_test(NAME microbit_qemu_sram
    COMMAND "${CMAKE_COMMAND}" "-DKICKOS_BUILD=${PROJECT_BINARY_DIR}"
            -P "${PROJECT_SOURCE_DIR}/tests/static/check_qemu_sram.cmake")
  kickos_host_gate(microbit_qemu_sram TIMEOUT 60)
endif()

# The NVIC line count of the MPS2 machine against the chip file's.
if(KICKOS_QEMU_MPS2 AND TARGET hello)
  kickos_add_qemu_test(TARGET hello NAME ${_tag}_nvic
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_nvic.sh"
    ARGS "${PROJECT_BINARY_DIR}/generated/include/kickos/chip_limits.h")
endif()
