# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 console DoS probe: an unprivileged thread with an MPU grant for the
# U0C1 window reroutes its receive interrupt onto the console's service-request node
# (SR0) and storms it. Measured on silicon: the console keeps beating -- a bounded CPU
# tax, not a denial of service (see main.cc). Diagnostic app: built only under
# KICKOS_ENABLE_SELFTEST (never a production image). Registered only for the
# xmc4800-relax board (chip-specific USIC access).
kickos_add_diagnostic_app(inprstorm
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET inprstorm)
  return() # diagnostics off -> app not built
endif()

# The chip register dir carries regs/usic.h (the shared USIC offset+bit-field header)
# -- definitions only, no code.
target_include_directories(inprstorm PRIVATE
  "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")

# No CTest gate: the result is an operator reading of the console under load on silicon
# (see main.cc), which the in-env sim/QEMU cannot reproduce.
