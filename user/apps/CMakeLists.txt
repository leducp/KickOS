# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_subdirectory(hello)
add_subdirectory(selftest)
add_subdirectory(mpu_fault)

# Board-specific bring-up demo: blinks an onboard LED (boards with a known LED).
if(KICKOS_BOARD STREQUAL "picopi"
   OR KICKOS_BOARD STREQUAL "f411disco"
   OR KICKOS_BOARD STREQUAL "bluepill"
   OR KICKOS_BOARD STREQUAL "f302nucleo"
   OR KICKOS_BOARD STREQUAL "due"
   OR KICKOS_BOARD STREQUAL "xmc4800")
  add_subdirectory(blink)
endif()
