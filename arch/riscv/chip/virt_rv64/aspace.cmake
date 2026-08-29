# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Translating-backend opt-in, the aspace half of the mpu.cmake axis. This chip's arch ships the
# arch_aspace_* family over the configured paging mode and its linker script carves the frame pool the page tables come
# from, so the kernel may build the frame allocator and the map editor's callers. The top
# CMakeLists includes this in its own scope, so a plain set (no PARENT_SCOPE) is what it reads; a
# chip whose Kconfig selects HAS_ASPACE and which ships no aspace.cmake is rejected there, so
# capability is an explicit opt-in.
#
# Validation status of this port: see docs/reference/boards.md.
set(KICKOS_CHIP_TRANSLATES ON)
