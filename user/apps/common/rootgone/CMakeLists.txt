# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(rootgone main.cc)
target_link_libraries(rootgone PRIVATE kickos)
kickos_emit_image(rootgone)
