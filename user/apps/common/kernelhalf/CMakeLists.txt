# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(kernelhalf main.cc)
target_link_libraries(kernelhalf PRIVATE kickos)
kickos_emit_image(kernelhalf)
