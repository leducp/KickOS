# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(mutexbench main.cc)
kickos_app_system(mutexbench COMPOSITION system.yaml)
kickos_emit_image(mutexbench)
