# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 USIC0-CH1 SSC hardware chip-select HOLD bench: proves on silicon whether
# MSLS/SELO stays asserted across a software-paced multi-word frame, and that
# PCR.FEM governs it -- the gate for a hardware CS in the XMC SPI service.
# Diagnostic app: built only under KICKOS_ENABLE_SELFTEST (never a production
# image). Registered only for the xmc4800-relax board (chip-specific USIC bring-up).
kickos_add_diagnostic_app(xmccshold
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET xmccshold)
  return() # diagnostics off -> app not built
endif()

# The chip register dir carries regs/usic.h (the shared USIC offset+bit-field
# header) -- definitions only, no code.
target_include_directories(xmccshold PRIVATE
  "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")

# No CTest gate: this app answers a HARDWARE question (does the CS stay held?) and
# is validated by the operator on silicon, not by the in-env sim/QEMU.
