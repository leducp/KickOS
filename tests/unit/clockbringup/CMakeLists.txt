# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Each chip port's clock bring-up, its own header compiled for the host over a register file
# (regfile.h) that times out the waits a case plants. seam/ is searched first: it holds the MMIO
# accessors, the bounded wait and the chip bases, whose real headers this target never reaches.
kickos_add_unit_test(NAME clock_bringup
  SOURCES regfile.cc esp32c6.cc mk64f.cc rx72m.cc sam3x8e.cc stm32f411.cc xmc4800.cc
  INCLUDES "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include")
target_include_directories(clock_bringup BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/seam")

# The RP2xxx bring-up is one header over each part's own family_map.h and regs/.
foreach(_part rp2040 rp2350)
  kickos_add_unit_test(NAME clock_bringup_${_part}
    SOURCES regfile.cc rp2xxx.cc
    INCLUDES "${PROJECT_SOURCE_DIR}"
    TEST_PREFIX "${_part}.")
  target_include_directories(clock_bringup_${_part} BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/seam" "${PROJECT_SOURCE_DIR}/arch/arm/chip/${_part}")
endforeach()
