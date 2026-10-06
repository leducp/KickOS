# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The packaged-driver framework's fixed shape. Structural, not configuration: no Kconfig symbol,
# no defconfig states one. Declared in the build, which reads them into the driver catalogue, and
# emitted to C through the generated <kickos/sys/driver_geometry.h>.

# What the shared bring-up creates for every packaged driver: its request endpoint, and the
# notification of a driver that uses one.
set(KICKOS_DRIVER_ENDPOINTS 1)
set(KICKOS_DRIVER_NOTIFICATIONS 1)

# The UART and USB CDC console classes' ring blocks, each one power-of-two grant. A driver of the
# class declares it as its BLOCK.
set(KICKOS_UART_BLOCK_SIZE 1024)
set(KICKOS_USB_BLOCK_SIZE 2048)
# The i.MX RT USB console's ring block, its controller's queue heads and transfer descriptors
# among it.
set(KICKOS_RT1062USB_BLOCK_SIZE 4096)
