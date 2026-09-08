# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(rootauth main.cc)
target_link_libraries(rootauth PRIVATE kickos)
kickos_emit_image(rootauth)
