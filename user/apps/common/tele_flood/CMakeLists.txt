# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(tele_flood main.cc)
target_link_libraries(tele_flood PRIVATE kickos)
kickos_emit_image(tele_flood)
