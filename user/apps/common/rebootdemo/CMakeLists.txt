# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(rebootdemo main.cc)
target_link_libraries(rebootdemo PRIVATE kickos)
kickos_emit_image(rebootdemo)
