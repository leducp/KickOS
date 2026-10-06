# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(fault main.cc)
target_link_libraries(fault PRIVATE kickos)
kickos_emit_image(fault)
