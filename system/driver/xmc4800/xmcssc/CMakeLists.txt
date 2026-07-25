# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_xmcssc: the XMC4800/USIC0-CH1 SSC unprivileged SPI driver (privileged
# bring-up + an unprivileged driver thread; clients call spi_transfer/spi_transact/
# spi_config -- see <kickos/driver/xmcssc.h>). REGDIR carries regs/usic.h (the
# shared USIC offset+bit-field header) -- definitions only, no code. No class leaf:
# it needs no shared register-logic object.
kickos_add_driver(xmcssc REGDIR arch/arm/chip/xmc4800)
