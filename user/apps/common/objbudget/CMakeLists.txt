# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(objbudget main.cc)
target_link_libraries(objbudget PRIVATE kickos)
kickos_emit_image(objbudget)
