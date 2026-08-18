# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_subdirectory(rxsci)

# kickos_i2c_rxriic: the RIICa backend of the I2C class <kickos/driver/i2c.h>, the public
# kos_i2c_* symbols, for a consumer that owns one RIIC channel window and clocks the bus
# inline. REGDIR carries the chip's clean-room regs/riic.h, register facts only.
#
# A service compiling this same engine renames its four class symbols at its own target
# (target_compile_definitions kos_i2c_*=<driver>_*). Missing the rename on ALL FOUR is not
# reported at link time; tests/static/check_class_backend.sh is what catches it.
kickos_add_driver(i2c_rxriic SOURCES i2c_riic.cc REGDIR arch/rx/chip/rx72m)
