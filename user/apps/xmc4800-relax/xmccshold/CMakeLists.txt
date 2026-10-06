# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 USIC0 channel 1 hardware chip-select hold bench over the window its composition grants:
# whether PCR.FEM governs MSLS across a software-paced frame. Judged from a silicon capture.
add_executable(xmccshold main.cc)
kickos_app_system(xmccshold COMPOSITION system.yaml)
kickos_emit_image(xmccshold)
kickos_app_judge(xmccshold tests/integration/check_xmccshold.sh)
# regs/usic.h: the shared USIC offsets and bit fields, definitions only.
target_include_directories(xmccshold PRIVATE "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")
