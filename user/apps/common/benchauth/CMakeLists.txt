# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(benchauth main.cc)
target_link_libraries(benchauth PRIVATE kickos)
kickos_emit_image(benchauth)
