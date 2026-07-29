# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 USIC0-CH1 "PV write only" probe (RM V1.3 Table 18-20): does an unprivileged
# thread holding an MPU grant for the channel window get its FDR/BRG/CCR writes into
# the register, or are they dropped/faulted? Diagnostic app: built only under
# KICKOS_ENABLE_SELFTEST (never a production image). Registered only for the
# xmc4800-relax board (chip-specific USIC access).
kickos_add_diagnostic_app(pvprobe
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET pvprobe)
  return() # diagnostics off -> app not built
endif()

# The chip register dir carries regs/usic.h (the shared USIC offset+bit-field header)
# -- definitions only, no code.
target_include_directories(pvprobe PRIVATE
  "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")

# No CTest gate: this app answers a HARDWARE question (USIC register access privilege)
# and is validated by the operator on silicon, not by the in-env sim/QEMU.
