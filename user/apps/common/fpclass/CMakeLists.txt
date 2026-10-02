# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(fpclass main.cc)
target_link_libraries(fpclass PRIVATE kickos)
kickos_emit_image(fpclass)
