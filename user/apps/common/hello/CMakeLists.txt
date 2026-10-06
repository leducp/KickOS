# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(hello main.cc)
target_link_libraries(hello PRIVATE kickos)
kickos_emit_image(hello)
