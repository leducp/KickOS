# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# GPIO direct-MMIO demo on the board's user LED, its port's window and `pinmux` granted by its
# composition. The parent builds it on the two boards whose GPIO layout main.cc carries.
add_executable(gpioblink main.cc)
kickos_app_system(gpioblink COMPOSITION systems/${KICKOS_BOARD}.yaml)
kickos_emit_image(gpioblink)
kickos_app_judge(gpioblink tests/integration/check_gpioblink.sh)
if(KICKOS_BOARD STREQUAL "frdmk64f")
  # PTB21, the blue LED, at MUX ALT1 = GPIO.
  target_compile_definitions(gpioblink PRIVATE KICKOS_GPIOBLINK_K64F=1
    KICKOS_GPIOBLINK_PORT=1 KICKOS_GPIOBLINK_PIN=21 KICKOS_GPIOBLINK_MUX=0x100
    KICKOS_GPIOBLINK_DEVICE="/dev/gpio/1")
elseif(KICKOS_BOARD STREQUAL "xmc4800-relax")
  # P5.8, LED2, at PC=0x10 = output push-pull general purpose.
  target_compile_definitions(gpioblink PRIVATE KICKOS_GPIOBLINK_XMC=1
    KICKOS_GPIOBLINK_PORT=5 KICKOS_GPIOBLINK_PIN=8 KICKOS_GPIOBLINK_MUX=0x10
    KICKOS_GPIOBLINK_DEVICE="/dev/port/5")
endif()
