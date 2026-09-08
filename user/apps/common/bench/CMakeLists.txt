# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(bench main.cc)
target_link_libraries(bench PRIVATE kickos)
kickos_emit_image(bench)
