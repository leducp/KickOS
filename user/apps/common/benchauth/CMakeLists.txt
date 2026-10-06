# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(benchauth main.cc)
kickos_app_system(benchauth COMPOSITION system.yaml)
kickos_emit_image(benchauth)
