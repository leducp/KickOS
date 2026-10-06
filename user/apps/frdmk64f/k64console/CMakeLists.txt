# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# K64F console demo: its composition names the packaged k64uart as stdout, and main and a worker
# print through it. Judged from a silicon capture.
add_executable(k64console main.cc)
kickos_app_system(k64console COMPOSITION system.yaml)
kickos_emit_image(k64console)
kickos_app_judge(k64console tests/integration/check_k64console.sh)

# Scramble-then-panic reclaim mode: an unprivileged thread holding no grant garbles UART0 through
# the slot the driver opened and panics, and the panic must still reach the wire, reclaimed out of
# the published console. Always defined: the app is built -Wundef -Werror.
option(K64CONSOLE_SCRAMBLE_TEST "k64console: scramble-then-panic console-reclaim test mode" OFF)
if(K64CONSOLE_SCRAMBLE_TEST)
  target_compile_definitions(k64console PRIVATE K64CONSOLE_SCRAMBLE_TEST=1)
else()
  target_compile_definitions(k64console PRIVATE K64CONSOLE_SCRAMBLE_TEST=0)
endif()
