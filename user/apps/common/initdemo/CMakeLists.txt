# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(initdemo main.cc)
target_link_libraries(initdemo PRIVATE kickos)
kickos_emit_image(initdemo)
