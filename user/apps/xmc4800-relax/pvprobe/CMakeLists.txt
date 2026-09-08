# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 USIC0-CH1 "PV write only" probe (RM V1.3 Table 18-20): does an unprivileged thread
# holding an MPU grant for the channel window get its FDR/BRG/CCR writes into the register, or
# are they dropped or faulted?
# No CTest gate: it answers a HARDWARE question (USIC register access privilege) and is
# validated by the operator on silicon.
kickos_add_app_target(pvprobe main.cc)
target_link_libraries(pvprobe PRIVATE kickos)
kickos_emit_image(pvprobe)

# The chip register dir carries regs/usic.h (the shared USIC offset plus bit-field header),
# definitions only, no code.
target_include_directories(pvprobe PRIVATE "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")
