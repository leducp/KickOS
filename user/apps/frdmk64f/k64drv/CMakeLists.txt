# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# K64F LPTMR0 driver over the window and line its composition grants: ten ticks of the 1 kHz LPO,
# then a thread holding no window reads the timer, which the AIPS slot lets through. Judged from
# a silicon capture.
add_executable(k64drv main.cc)
kickos_app_system(k64drv COMPOSITION system.yaml)
kickos_emit_image(k64drv)
kickos_app_judge(k64drv tests/integration/check_k64drv.sh)
