# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(fault main.cc)
target_link_libraries(fault PRIVATE kickos)
kickos_emit_image(fault)
