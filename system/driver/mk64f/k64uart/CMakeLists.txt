# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_k64uart: the K64F/UART0 userspace polled UART TX console driver (an
# unprivileged driver thread + a privileged one-shot handover bring-up; see
# <kickos/driver/k64uart.h>). The per-byte transmit-ready poll is shared UART
# register logic, so it links the kickos_class_mk64f leaf, not a local copy.
kickos_add_driver(k64uart CLASS kickos_class_mk64f)
