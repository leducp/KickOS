# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 console DoS probe: an unprivileged thread with an MPU grant for the
# U0C1 window reroutes its receive interrupt onto the console's service-request node
# (SR0) and storms it. Measured on silicon at the 115.2 kHz divider profile: the console
# keeps beating, a bounded CPU tax rather than a denial of service (see main.cc).
# Diagnostic app: built only under KICKOS_ENABLE_SELFTEST (never a production image).
# Registered only for the xmc4800-relax board (chip-specific USIC access).
#
# THREE targets from one source. FDR/BRG are allowlist entries, so the event rate is the
# attacker's to choose and the storm has to be re-measurable at the divider ceiling with
# the old operating point kept as the comparison. TBCTR and the TBUFx aperture are U,PV
# (RM Table 18-20), so the FIFO profile arms an autonomous drain with no seam entry.
# Same code, one define apart:
#   inprstorm      the 115.2 kHz profile (PDIV+1=14, DCTQ+1=16, STEP=367)
#   inprstormmax   every divider at its minimum (PDIV+1=1, DCTQ+1=1, STEP=1023)
#   inprstormfifo  TX FIFO autonomous drain, forcing the max-rate divider
foreach(_t inprstorm inprstormmax inprstormfifo)
  kickos_add_diagnostic_app(${_t}
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  if(NOT TARGET ${_t})
    return() # diagnostics off -> app not built
  endif()
  # The chip register dir carries regs/usic.h (the shared USIC offset+bit-field header),
  # definitions only, no code.
  target_include_directories(${_t} PRIVATE
    "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")
endforeach()
target_compile_definitions(inprstormmax PRIVATE INPRSTORM_MAX_RATE=1)
target_compile_definitions(inprstormfifo PRIVATE INPRSTORM_FIFO=1)

# No CTest gate: the result is an operator reading of the console under load on silicon
# (see main.cc), which the in-env sim/QEMU cannot reproduce.
