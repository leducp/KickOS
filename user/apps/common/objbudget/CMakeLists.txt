# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(objbudget main.cc)
target_link_libraries(objbudget PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(objbudget)
