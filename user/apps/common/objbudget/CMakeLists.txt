# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(objbudget main.cc)
target_link_libraries(objbudget PRIVATE kickos)
kickos_emit_image(objbudget)
