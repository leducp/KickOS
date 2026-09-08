# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(errnoprobe main.cc)
target_link_libraries(errnoprobe PRIVATE kickos)
kickos_emit_image(errnoprobe)
