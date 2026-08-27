# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The capability table's STRUCTURAL constants. None is configuration: nothing selects one,
# no option's availability or default depends on one, so none is a Kconfig symbol and no
# defconfig can state one.
#
# They live in the BUILD and reach C through the generated config/cap_width.h, and the
# direction is the point: every term of the configure-time width sum has to be a number
# CMake holds, and the only way back out of C is a preprocessor probe. Moving one of these
# into a kernel header would put that probe back.

# The first index an own-create may take. The well-known indices below it are kernel
# policy and are named by <kickos/sys/cap_index.h>, which takes the number from here.
# A renumber may only go DOWNWARD, only for a slot NOTHING seats, and is an ABI break.
set(KICKOS_CAP_FIRST_DYNAMIC 2)

# The storage granule. armv6m has no divide instruction, so it must stay a power of two.
# Changing it moves KCAP_CHUNK_SHIFT (cap.h), which boards compile the flat decode, and so
# the cap_chunk_span PARTIAL list in user/apps/common/selftest/CMakeLists.txt.
set(KICKOS_CAP_CHUNK_TARGET 8)

# The runs held by something that is NOT a thread-pool slot: just the one thread_create_call holds
# in its ThreadAttr until thread_create takes it over. Root's slot accounts for root's run,
# and idle holds none. A new kind of holder is a term here.
set(KICKOS_CAP_RUN_OFF_POOL 1)
