# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The alignment a thread's stack, its base and its size share. Structural, not configuration:
# no Kconfig symbol, no defconfig states one. Declared in the build, which exports it in the
# manifest and hands it to the link, and emitted to C through the generated
# config/stack_geometry.h.
set(KICKOS_STACK_ALIGN 16)
