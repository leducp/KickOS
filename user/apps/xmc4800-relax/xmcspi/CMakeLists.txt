# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 USIC0-CH1 SSC (SPI) internal-loopback driver over the MMIO-grant seam: the
# PMSA-proven board-support / userspace-library SPI reference (the XMC counterpart of
# user/apps/f411disco/f411spi), proven both ways (granted works, ungranted faults MemManage).
# No CTest gate: it answers a HARDWARE question (PMSA peripheral enforcement) and is validated
# by the operator on silicon.
kickos_add_app_target(xmcspi main.cc)
target_link_libraries(xmcspi PRIVATE kickos)
kickos_emit_image(xmcspi)

# The chip register dir carries regs/usic.h (the shared USIC offset plus bit-field header),
# definitions only, no code.
target_include_directories(xmcspi PRIVATE "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")
