# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(aspaceufault main.cc)
target_link_libraries(aspaceufault PRIVATE kickos)
kickos_emit_image(aspaceufault)
