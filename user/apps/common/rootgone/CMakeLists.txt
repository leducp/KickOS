# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(rootgone main.cc)
target_link_libraries(rootgone PRIVATE kickos)
kickos_emit_image(rootgone)
