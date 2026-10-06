# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(rootauth main.cc)
kickos_app_system(rootauth COMPOSITION system.yaml)
kickos_emit_image(rootauth)
