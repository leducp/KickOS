# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(initdemo main.cc)
kickos_app_system(initdemo COMPOSITION system.yaml)
kickos_emit_image(initdemo)
