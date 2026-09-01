# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Espressif ESP32: dual-core Xtensa LX6, windowed ABI.
#
# KICKOS_CPU rather than KICKOS_MCPU: no compiler flag selects the core on this family, the
# toolchain's own overlay fixing it, so this names the core and contributes nothing to the
# command line. Read where that name is consumed rather than pre-project().
set(KICKOS_CPU "lx6")
