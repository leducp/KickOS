# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_c6uart: the ESP32-C6/UART0 buffered userspace UART driver (a privileged one-shot
# bring-up plus two unprivileged threads on the <kickos/sys/uart_service.h> substrate).
# REGDIR carries regs/uart.h -- register offsets and bit fields only, no code. No class
# leaf: nothing here is shared with the kernel side.
kickos_add_driver(c6uart REGDIR arch/riscv/chip/esp32c6)
