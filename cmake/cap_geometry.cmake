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
set(KICKOS_CAP_FIRST_DYNAMIC 1)

# The storage granule. armv6m has no divide instruction, so it must stay a power of two.
# Changing it moves KCAP_CHUNK_SHIFT (cap.h), and so which boards compile the flat decode.
set(KICKOS_CAP_CHUNK_TARGET 8)

# The widest capability table one thread may seat, in slots: whole chunks, and within the byte
# the kernel counts a table's IRQ capabilities in (KCAP_WIDTH_CEILING, cap.h).
set(KICKOS_CAP_WIDTH_MAX 248)

# The most concurrent inbound reply capabilities one thread may hold. It reserves no slot, and
# a thread that may hold none could never serve a kos_call.
set(KICKOS_CAP_REPLY_MAX 1)

# The bytes of one capability entry. cap.h asserts sizeof(CapEntry) against it.
set(KICKOS_CAP_ENTRY_BYTES 16)
