# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(benchauth main.cc)
target_link_libraries(benchauth PRIVATE kickos)
kickos_emit_image(benchauth)
