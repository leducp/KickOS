# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 USIC privileged-write probe over the channel window its composition grants: which
# registers drop an unprivileged store, the seam writing them, its refusals, and an ungranted SCU
# read that must fault. Judged from a silicon capture.
add_executable(pvprobe main.cc)
kickos_app_system(pvprobe COMPOSITION system.yaml)
kickos_emit_image(pvprobe)
kickos_app_judge(pvprobe tests/integration/check_pvprobe.sh)
# regs/usic.h: the shared USIC offsets and bit fields, definitions only.
target_include_directories(pvprobe PRIVATE "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")
