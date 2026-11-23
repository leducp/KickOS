# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(callrace main.cc)
kickos_app_system(callrace COMPOSITION system.yaml)
kickos_emit_image(callrace)
