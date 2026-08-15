# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# GPIO direct-MMIO demo (M4.3): the kernel does NOT do GPIO. A privileged bring-up main
# grants the LED port's register block to an UNPRIVILEGED worker as a spawn MMIO window;
# the worker toggles the pin by writing that window DIRECTLY (no syscall per edge). The
# pin is muxed by the default init's pin-map step (the clock->pinmux->gpio DAG); this app
# only drives it. Board-agnostic C++ -- it builds on EVERY arch; a chip whose register
# layout is not known here builds a park-only stub (no GPIO block to grant).
kickos_add_application(gpioblink
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# Demo pin per board (matches the board pin-map in system/init/<board>/pinmap.cc): the demo
# LED, NOT the kernel diag LED. Default 5,8 is harmless on a board with no layout.
if(KICKOS_BOARD STREQUAL "frdmk64f")
  set(_gpioblink_port 1)   # PTB21 = blue LED (muxed ALT1 = GPIO by the K64F pin-map)
  set(_gpioblink_pin 21)
else()
  set(_gpioblink_port 5)   # xmc4800-relax: P5.8 = LED2; also the harmless default elsewhere
  set(_gpioblink_pin 8)
endif()
target_compile_definitions(gpioblink PRIVATE
  KICKOS_GPIOBLINK_PORT=${_gpioblink_port}
  KICKOS_GPIOBLINK_PIN=${_gpioblink_pin})

# Per-chip register-layout selector: the worker drives the granted window directly, so it
# needs the chip's port-block offsets. Any other chip -> park-only stub (builds everywhere).
if(KICKOS_CHIP STREQUAL "xmc4800")
  target_compile_definitions(gpioblink PRIVATE KICKOS_GPIOBLINK_XMC=1)
elseif(KICKOS_CHIP STREQUAL "mk64f")
  target_compile_definitions(gpioblink PRIVATE KICKOS_GPIOBLINK_K64F=1)
endif()
