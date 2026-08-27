# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Host arms for the physical frame allocator. It touches no hardware and no kernel state, so
# it is gated here rather than waiting for a board to boot, which matters because it is the
# largest new subsystem of the translation work and every later step builds on it.
#
# Takes the kernel target's include path and definitions verbatim, as ktime does: this TU
# compiles a kernel source, and a divergent config here would gate a different translation
# unit than the one that ships.
kickos_add_unit_test(NAME frame_alloc
  SOURCES frame_alloc.cc "${PROJECT_SOURCE_DIR}/kernel/mem/frame.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
