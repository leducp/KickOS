# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(rebootdemo main.cc)
kickos_app_system(rebootdemo COMPOSITION system.yaml)
kickos_emit_image(rebootdemo)
