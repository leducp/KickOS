# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ESP32-C6 GPIO10 blink over the pin bank its composition grants, then a child thread's write to
# the same block outside that window, which PMP must refuse. Judged from a silicon capture.
add_executable(c6blink main.cc)
# The GPIO output-latch readback is shared register logic, from the freestanding class leaf.
target_link_libraries(c6blink PRIVATE kickos_class_esp32c6)
kickos_app_system(c6blink COMPOSITION system.yaml)
kickos_emit_image(c6blink)
kickos_app_judge(c6blink tests/integration/check_c6blink.sh)
