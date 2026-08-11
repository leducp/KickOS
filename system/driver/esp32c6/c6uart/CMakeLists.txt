# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_c6uart: the ESP32-C6 UART0 buffered userspace UART driver - a privileged one-shot
# bring-up plus two unprivileged threads on the <kickos/sys/uart_service.h> substrate.
# REGDIR carries regs/uart.h + irq.h + mmap.h, register facts only, no code. No class leaf:
# nothing here is shared with the kernel side.
#
# TWO SOURCES, and the split is the driver-model ruling: uart_c6.cc is the C6 backend of the
# raw UART class <kickos/driver/uart.h> and owns every register; c6uart.cc is the service
# composed on it. An image gets exactly one definition of that class, so a second esp32c6
# UART driver may not be linked beside this one.
kickos_add_driver(c6uart SOURCES c6uart.cc uart_c6.cc REGDIR arch/riscv/chip/esp32c6)
