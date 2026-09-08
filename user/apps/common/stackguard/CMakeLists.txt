# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(stackguard main.cc)
target_link_libraries(stackguard PRIVATE kickos)
kickos_emit_image(stackguard)
