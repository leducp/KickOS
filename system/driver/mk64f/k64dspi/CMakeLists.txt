# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_k64dspi: the K64F/DSPI0 unprivileged SPI driver (privileged bring-up + an
# unprivileged driver thread; clients call spi_transfer/spi_enable_cs/spi_disable_cs
# -- see <kickos/driver/k64dspi.h>). The DSPI RX-FIFO fill-level read is shared
# register logic, so it links the kickos_class_mk64f leaf, not a local copy.
kickos_add_driver(k64dspi CLASS kickos_class_mk64f)
