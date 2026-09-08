# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(rootfault main.cc)
target_link_libraries(rootfault PRIVATE kickos)
kickos_emit_image(rootfault)
