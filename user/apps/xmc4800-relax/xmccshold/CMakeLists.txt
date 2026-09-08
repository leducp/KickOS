# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 USIC0-CH1 SSC hardware chip-select HOLD bench: proves on silicon whether MSLS/SELO
# stays asserted across a software-paced multi-word frame, and that PCR.FEM governs it. That is
# the gate for a hardware CS in the XMC SPI service.
# No CTest gate: it answers a HARDWARE question (does the CS stay held?) and is validated by
# the operator on silicon.
kickos_add_app_target(xmccshold main.cc)
target_link_libraries(xmccshold PRIVATE kickos)
kickos_emit_image(xmccshold)

# The chip register dir carries regs/usic.h (the shared USIC offset plus bit-field header),
# definitions only, no code.
target_include_directories(xmccshold PRIVATE "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")
