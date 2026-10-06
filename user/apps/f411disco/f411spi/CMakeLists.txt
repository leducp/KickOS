# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# STM32F411 SPI1 loopback over the window and line its composition grants, then an ungranted
# GPIOB read that must fault. Needs a PA7-to-PA6 jumper; judged from a silicon capture.
add_executable(f411spi main.cc)
kickos_app_system(f411spi COMPOSITION system.yaml)
kickos_emit_image(f411spi)
kickos_app_judge(f411spi tests/integration/check_f411spi.sh)
