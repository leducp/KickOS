# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_lx6uart: the ESP32 (Xtensa LX6) UART0 buffered userspace UART driver -- a
# privileged one-shot bring-up plus two unprivileged threads on the
# <kickos/sys/uart_service.h> substrate. REGDIR carries regs/uart.h + irq.h + mmap.h,
# register facts only, no code. No class leaf: nothing here is shared with the kernel side.
kickos_add_driver(lx6uart REGDIR arch/xtensa/chip/esp32)
