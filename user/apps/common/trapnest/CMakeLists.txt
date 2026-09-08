# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The NESTED-TRAP witness. rv32imac only: it is the one backend that runs syscall dispatch
# on the caller's own stack AND builds its trap frames in software, so it is the only one
# where an interrupt taken mid-dispatch can put a frame at an unbounded depth on a thread's
# stack. rxv3 switches to the ISP on interrupt acceptance and armv7m forces SP_main in
# handler mode, so on both the nested frame lands on a trusted stack by hardware. main.cc
# #errors if an image is built elsewhere.
#
# The witness the arm reads is kept OUT of a bench build: its call site is inside what the
# injected-IRQ bench measures. So is the arm, which would report traps=0 and fail for a
# reason that is not a regression.
kickos_add_app_target(trapnest main.cc)
target_link_libraries(trapnest PRIVATE kickos)
kickos_emit_image(trapnest)

# arch-private include path, on this target only.
target_include_directories(trapnest PRIVATE
  "${PROJECT_SOURCE_DIR}/arch/riscv/rv32imac/include")
