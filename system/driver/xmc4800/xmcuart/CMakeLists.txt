# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_xmcuart: the XMC4800 userspace polled UART TX console driver (an
# unprivileged driver thread + a privileged one-shot handover bring-up; see
# <kickos/driver/xmcuart.h>). REGDIR carries regs/usic.h (the shared USIC
# offset+bit-field header) -- definitions only, no code. The per-byte transmit-ready
# poll is shared USIC register logic, so it links the kickos_class_xmc4800 leaf.
kickos_add_driver(xmcuart CLASS kickos_class_xmc4800 REGDIR arch/arm/chip/xmc4800)
