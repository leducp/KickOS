# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(fp_switch main.cc)
target_link_libraries(fp_switch PRIVATE kickos)
kickos_emit_image(fp_switch)
