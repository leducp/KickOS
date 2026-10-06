# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 USIC0 channel 1 SSC internal loopback over the window and line its composition grants,
# then an ungranted SCU read that must fault. Judged from a silicon capture.
add_executable(xmcspi main.cc)
kickos_app_system(xmcspi COMPOSITION system.yaml)
kickos_emit_image(xmcspi)
kickos_app_judge(xmcspi tests/integration/check_xmcspi.sh)
# regs/usic.h: the shared USIC offsets and bit fields, definitions only.
target_include_directories(xmcspi PRIVATE "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")
