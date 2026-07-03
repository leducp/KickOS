# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Bring-up smoke test: blinks an onboard LED. The LED wiring is board-specific
# (direct MMIO), selected by a -DKICKOS_BLINK_<board> define. Only added for
# boards with a known LED (see user/apps/CMakeLists.txt).
kickos_add_application(blink
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(KICKOS_BOARD STREQUAL "picopi")
  target_compile_definitions(blink PRIVATE KICKOS_BLINK_PICOPI)
elseif(KICKOS_BOARD STREQUAL "f411disco")
  target_compile_definitions(blink PRIVATE KICKOS_BLINK_F411DISCO)
elseif(KICKOS_BOARD STREQUAL "bluepill")
  target_compile_definitions(blink PRIVATE KICKOS_BLINK_BLUEPILL)
elseif(KICKOS_BOARD STREQUAL "f302nucleo")
  target_compile_definitions(blink PRIVATE KICKOS_BLINK_F302NUCLEO)
elseif(KICKOS_BOARD STREQUAL "due")
  target_compile_definitions(blink PRIVATE KICKOS_BLINK_DUE)
elseif(KICKOS_BOARD STREQUAL "xmc4800")
  target_compile_definitions(blink PRIVATE KICKOS_BLINK_XMC4800)
endif()
