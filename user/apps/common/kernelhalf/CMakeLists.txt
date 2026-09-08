# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(kernelhalf main.cc)
target_link_libraries(kernelhalf PRIVATE kickos)
kickos_emit_image(kernelhalf)
