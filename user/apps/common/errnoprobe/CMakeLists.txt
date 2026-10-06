# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(errnoprobe main.cc)
target_link_libraries(errnoprobe PRIVATE kickos)
kickos_emit_image(errnoprobe)
