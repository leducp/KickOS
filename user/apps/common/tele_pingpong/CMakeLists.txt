# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(tele_pingpong main.cc)
target_link_libraries(tele_pingpong PRIVATE kickos)
kickos_emit_image(tele_pingpong)
