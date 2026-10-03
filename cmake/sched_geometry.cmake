# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The scheduler's priority range. Structural, not configuration: no Kconfig symbol, no defconfig
# states one. Declared in the build, which exports it in the manifest, and emitted to C through
# the generated config/priorities.h.
#
# Priority 0 is the idle thread's and a higher number is a higher priority (find-first-set on
# the ready bitmap). KICKOS_NUM_PRIO is coupled to the uint32_t ready bitmap: raising it past 32
# needs a hierarchical bitmap, not a bigger number.
set(KICKOS_NUM_PRIO 32)
set(KICKOS_PRIO_IDLE 0)
set(KICKOS_PRIO_MIN 1)
math(EXPR KICKOS_PRIO_MAX "${KICKOS_NUM_PRIO} - 1")
# The kernel creates root at KICKOS_PRIO_MAX. An image linking no system target lowers root to
# this one before its constructors; a composed init lowers itself to its composition's.
math(EXPR KICKOS_PRIO_ROOT "${KICKOS_PRIO_MIN} + 1")
