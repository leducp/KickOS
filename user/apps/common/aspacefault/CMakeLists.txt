# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(aspacefault main.cc)
target_link_libraries(aspacefault PRIVATE kickos)
kickos_emit_image(aspacefault)
