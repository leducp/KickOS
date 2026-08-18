# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 USIC0-CH1 SSC (SPI) internal-loopback driver over the MMIO-grant seam:
# the PMSA-proven board-support / userspace-library SPI reference (the XMC
# counterpart of user/apps/f411disco/f411spi), proven both ways (granted works, ungranted
# faults MemManage). Diagnostic app: built only under KICKOS_ENABLE_SELFTEST
# (never a production image). Registered only for the xmc4800-relax board
# (chip-specific USIC/SCU bring-up).
kickos_add_diagnostic_app(xmcspi
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET xmcspi)
  return() # diagnostics off -> app not built
endif()

# The chip register dir carries regs/usic.h (the shared USIC offset+bit-field
# header) -- definitions only, no code.
target_include_directories(xmcspi PRIVATE
  "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")

# No CTest gate: this app answers a HARDWARE question (PMSA peripheral enforcement)
# and is validated by the operator on silicon, not by the in-env sim/QEMU.
